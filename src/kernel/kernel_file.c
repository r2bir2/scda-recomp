/*
 * kernel_file.c - Xbox File I/O
 *
 * Implements the Nt*File kernel functions. All Xbox device paths are
 * translated through kernel_path.c before use.
 *
 * The Xbox kernel uses NT-style file I/O with ANSI strings in
 * OBJECT_ATTRIBUTES (unlike Windows NT, which uses Unicode).
 *
 * Two backends:
 *   _WIN32  -> Win32 CreateFileW / ReadFile / FindFirstFileW ...
 *   POSIX   -> open / read / write / stat / opendir ...
 * The Xbox semantics (disposition mapping, IO_STATUS_BLOCK, info classes)
 * are identical on both; only the host syscalls differ.
 */

#define _GNU_SOURCE   /* FNM_CASEFOLD */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* xbox_PrepareHost* (docs Update 493) */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fnmatch.h>
#endif

/* Get the ANSI path from OBJECT_ATTRIBUTES (platform-independent). */
static const char* get_xbox_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!ObjectAttributes || !ObjectAttributes->ObjectName ||
        !ObjectAttributes->ObjectName->Buffer)
        return NULL;
    return ObjectAttributes->ObjectName->Buffer;
}

/* ======================================================================== */
#if defined(_WIN32) || defined(XBOX_POSIX_WIN32_SHIM)
/* ====================  Win32 backend  =================================== */
/* ======================================================================== */

/* Opt-in, low-volume timing probe for mission-cache I/O versus Bink/GPU.
 * Uses the same QPC timebase as [BINK-FRAME]. It does not change I/O status. */
static int scda_io_timing_on(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RECOMP_SCDA_IO_TIMING") != NULL;
    return enabled;
}

static void scda_io_timing_record(char op, HANDLE handle, ULONG want, DWORD got,
                                  LARGE_INTEGER begin, LARGE_INTEGER prepared,
                                  LARGE_INTEGER done)
{
    static LARGE_INTEGER hz;
    static volatile LONG64 next_summary;
    static volatile LONG64 reads, writes, read_bytes, write_bytes;
    static volatile LONG64 read_prep_us, read_host_us;
    static volatile LONG64 write_prep_us, write_host_us;
    LONGLONG prep_us, host_us;
    if (!hz.QuadPart)
        QueryPerformanceFrequency(&hz);
    if (!hz.QuadPart)
        return;
    prep_us = (prepared.QuadPart - begin.QuadPart) * 1000000 / hz.QuadPart;
    host_us = (done.QuadPart - prepared.QuadPart) * 1000000 / hz.QuadPart;
    if (op == 'R') {
        InterlockedIncrement64(&reads);
        InterlockedExchangeAdd64(&read_bytes, got);
        InterlockedExchangeAdd64(&read_prep_us, prep_us);
        InterlockedExchangeAdd64(&read_host_us, host_us);
    } else {
        InterlockedIncrement64(&writes);
        InterlockedExchangeAdd64(&write_bytes, got);
        InterlockedExchangeAdd64(&write_prep_us, prep_us);
        InterlockedExchangeAdd64(&write_host_us, host_us);
    }
    if (prep_us + host_us >= 5000) {
        char path[512] = {0};
        GetFinalPathNameByHandleA(handle, path, sizeof(path), FILE_NAME_NORMALIZED);
        fprintf(stderr,
                "[SCDA-IO-SLOW] qpc=%lld tid=%lu op=%c handle=%p "
                "want=%lu got=%lu prep_us=%lld host_us=%lld path=\"%s\"\n",
                (long long)done.QuadPart, (unsigned long)GetCurrentThreadId(),
                op, handle, (unsigned long)want, (unsigned long)got,
                (long long)prep_us, (long long)host_us, path);
    }
    {
        LONGLONG next = next_summary;
        if (done.QuadPart >= next &&
            InterlockedCompareExchange64(&next_summary,
                                         done.QuadPart + 5 * hz.QuadPart,
                                         next) == next) {
            fprintf(stderr,
                    "[SCDA-IO-RATE] qpc=%lld hz=%lld reads=%lld rbytes=%lld "
                    "writes=%lld wbytes=%lld "
                    "rprep_us=%lld rhost_us=%lld "
                    "wprep_us=%lld whost_us=%lld\n",
                    (long long)done.QuadPart, (long long)hz.QuadPart,
                    (long long)InterlockedExchange64(&reads, 0),
                    (long long)InterlockedExchange64(&read_bytes, 0),
                    (long long)InterlockedExchange64(&writes, 0),
                    (long long)InterlockedExchange64(&write_bytes, 0),
                    (long long)InterlockedExchange64(&read_prep_us, 0),
                    (long long)InterlockedExchange64(&read_host_us, 0),
                    (long long)InterlockedExchange64(&write_prep_us, 0),
                    (long long)InterlockedExchange64(&write_host_us, 0));
        }
    }
}

/* Convert Xbox create disposition to Win32 */
static DWORD xbox_disposition_to_win32(ULONG Disposition)
{
    switch (Disposition) {
        case XBOX_FILE_SUPERSEDE:    return CREATE_ALWAYS;
        case XBOX_FILE_OPEN:         return OPEN_EXISTING;
        case XBOX_FILE_CREATE:       return CREATE_NEW;
        case XBOX_FILE_OPEN_IF:      return OPEN_ALWAYS;
        case XBOX_FILE_OVERWRITE:    return TRUNCATE_EXISTING;
        case XBOX_FILE_OVERWRITE_IF: return CREATE_ALWAYS;
        default:                     return OPEN_EXISTING;
    }
}

/* Convert Xbox access mask to Win32 */
static DWORD xbox_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)           result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)          result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)            result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)         result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)        result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)       result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)   result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES)  result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)            result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                  result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

/* Convert Xbox share access to Win32 */
static DWORD xbox_share_to_win32(ULONG Share)
{
    DWORD result = 0;
    if (Share & 0x01) result |= FILE_SHARE_READ;
    if (Share & 0x02) result |= FILE_SHARE_WRITE;
    if (Share & 0x04) result |= FILE_SHARE_DELETE;
    return result;
}

/* Translate an Xbox OBJECT_ATTRIBUTES path to a Win32 wide path */
static BOOL translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                               WCHAR* win_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path)
        return FALSE;
    return xbox_translate_path(xbox_path, win_path, buf_size);
}

/* DynamicXbox.umd read-tracing (2026-09-02, SCDA UMOD archive-layer
 * investigation -- see docs/SCDA_WORKLOG.md). Small side-table mapping
 * open HANDLEs known to be the .umd archive so xbox_NtReadFile can log
 * offset/length/leading-bytes without threading a path through every
 * caller. Diagnostic-only; not meant to stay enabled long-term. */
#define SCDA_UMD_TRACK_MAX 8
static HANDLE s_scda_umd_handles[SCDA_UMD_TRACK_MAX];
static int s_scda_umd_handle_count = 0;

static void scda_umd_track_handle(HANDLE h, const WCHAR *win_path)
{
    size_t len = wcslen(win_path);
    if (len < 4 || _wcsicmp(win_path + len - 4, L".umd") != 0)
        return;
    if (s_scda_umd_handle_count < SCDA_UMD_TRACK_MAX)
        s_scda_umd_handles[s_scda_umd_handle_count++] = h;
#if defined(XBOX_POSIX_WIN32_SHIM)
    w32_fprintf(stderr, "[SCDA-UMD-OPEN] handle=%p path=%S\n", h, win_path);
#else
    fprintf(stderr, "[SCDA-UMD-OPEN] handle=%p path=%S\n", h, win_path);
#endif
    fflush(stderr);
}

static int scda_umd_is_tracked(HANDLE h)
{
    int i;
    for (i = 0; i < s_scda_umd_handle_count; i++)
        if (s_scda_umd_handles[i] == h)
            return 1;
    return 0;
}

/* RECOMP_SKIP_INTRO=1 (scda_options.txt SkipIntro, docs Update
 * 614): as the game reads its System/SplinterCell4Xbox.ini out of the UMD
 * archive, name boot videos that do not exist. The game's own missing-file
 * path then skips the logo film and the intro trailer and carries on to the
 * menu background (isolated runs, Update 607; values as the experimental
 * IPS in projects/scda/patches). Same length, in the read buffer only --
 * no file on disc or in the cache changes. */
static void scda_skip_intro_patch(unsigned char *buf, DWORD len)
{
    static int on = -1;
    static const struct { const char *from, *to; } rep[] = {
        { "LogosVideo=logos.bik", "LogosVideo=nope!.bik" },
        { "SplashVideo=teaser.bik", "SplashVideo=nope!!.bik" },
    };
    if (on < 0) {
        const char *e = getenv("RECOMP_SKIP_INTRO");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    for (size_t k = 0; k < sizeof(rep) / sizeof(rep[0]); k++) {
        size_t n = strlen(rep[k].from);
        for (DWORD i = 0; i + n <= len; i++) {
            if (buf[i] == (unsigned char)rep[k].from[0] &&
                !memcmp(buf + i, rep[k].from, n)) {
                memcpy(buf + i, rep[k].to, n);
                fprintf(stderr, "[OPTIONS] intro skipped: %s -> %s\n",
                        rep[k].from, rep[k].to);
            }
        }
    }
}

/* Disc-backed (D:\, \Device\CdRom0\, Z:\) file overrun reads (2026-09-17,
 * SCDA common.lin ring-buffer corruption -- see docs/SCDA_WORKLOG.md Updates
 * 173/174). Real Xbox disc files are extracted onto the host filesystem as
 * exact-length standalone files, but titles routinely round a read up to a
 * fixed chunk size and let the drive deliver whatever real bytes follow the
 * file's logical end on the disc (XISO packs file data and directory tables
 * back-to-back with no gap). A host ReadFile() on the standalone extracted
 * file has nothing there and quite correctly reports a short read, which the
 * title's own ring-buffer code does not handle (it advances its write cursor
 * as if the full chunk had landed, then parses whatever sits past the short
 * read as if it were more stream data).
 *
 * The first attempt at a fix (Update 173) topped a short read up with the
 * real bytes that follow on the original disc image, since that is what
 * real hardware would deliver. Byte-exact right, but it did not help: those
 * real trailing bytes are the XISO directory table for the same folder, not
 * more stream content, and the title's own chunk-header parser -- which was
 * never meant to see them at all, real hardware included -- reads them as an
 * enormous bogus "next chunk size" and corrupts the same way. What the
 * parser DOES already tolerate, confirmed live over 16 consecutive
 * iterations before this fix existed, is a run of zero bytes: a zero chunk
 * size just gets consumed 8 bytes at a time without touching anything it
 * shouldn't. Zero-filling the overrun is therefore the safer of the two and
 * the one actually kept: it lets the ring buffer stay internally consistent
 * (write cursor and real content agree again) without handing the parser
 * data shaped like something it should act on.
 *
 * Only a genuinely PARTIAL read (some real bytes, then the file's true end)
 * gets topped up -- a read that starts already past the end (zero real
 * bytes) is left to report EOF/failure exactly as before. The bug this
 * fixes is specifically the write cursor advancing past a short read as if
 * the full chunk had landed; once a handle has had that one topped-up
 * "transition" read, every later read from the same exhausted file has
 * nothing wrong left to correct, and manufacturing further chunks of pure
 * zero content for it every time it is asked for more would just trade the
 * crash for a request loop that never terminates on its own (confirmed
 * live: several hundred phantom chunks with no sign of stopping). */
#define SCDA_DISC_TRACK_MAX 1024
static HANDLE s_scda_disc_handles[SCDA_DISC_TRACK_MAX];
static int s_scda_disc_handle_count = 0;

static int scda_has_prefix_ci(const char *path, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i]; i++) {
        if (!path[i] || tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

static int scda_path_is_disc_backed(const char *xbox_path)
{
    if (!xbox_path) return 0;
    if (scda_has_prefix_ci(xbox_path, "D:\\")) return 1;
    if (scda_has_prefix_ci(xbox_path, "Z:\\")) return 1;
    if (scda_has_prefix_ci(xbox_path, "\\Device\\CdRom0\\")) return 1;
    if (scda_has_prefix_ci(xbox_path, "\\??\\D:\\")) return 1;
    return 0;
}

static void scda_disc_track_handle(HANDLE h, const char *xbox_path)
{
    if (!scda_path_is_disc_backed(xbox_path))
        return;
    if (s_scda_disc_handle_count >= SCDA_DISC_TRACK_MAX)
        return;
    s_scda_disc_handles[s_scda_disc_handle_count++] = h;
    /* Diagnostic only (2026-09-17, video-stall investigation): XBOX_TRACE
     * is compiled out in RelWithDebInfo, so this is the only way to see
     * which disc-backed file a later "disc overrun read" line belongs to. */
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
             "disc-backed handle tracked: %s", xbox_path);
}

static int scda_disc_is_tracked(HANDLE h)
{
    int i;
    for (i = 0; i < s_scda_disc_handle_count; i++)
        if (s_scda_disc_handles[i] == h)
            return 1;
    return 0;
}

/* Zero the unwritten tail of a short read on a disc-backed handle, so that
 * if the title's own ring-buffer bookkeeping ever advances its cursor past
 * the bytes actually delivered (Update 173: it does, on a formula that
 * ignores the real transfer count), what it finds there is pristine zero
 * memory -- the same thing real hardware's own untouched RAM would hold --
 * rather than leftover host heap content. This is memory hygiene only.
 *
 * It deliberately does NOT change how many bytes NtReadFile reports back
 * (see the caller): live xemu ground-truth tracing (Update 185) proved
 * real hardware experiences this exact same short read (same want, same
 * got, same file position) and the title's own stream logic reads that
 * short count and stops cleanly on its own -- it does not need, and must
 * not be denied, the genuine short-read signal. The earlier version of
 * this function additionally inflated the reported byte count to make the
 * read look fully satisfied, which hid that signal and is what actually
 * caused the title to keep asking for one chunk too many. */
static void scda_disc_zero_short_read_tail(HANDLE file, unsigned char *buffer,
                                            DWORD have, DWORD want)
{
    DWORD need;

    /* Update 302 diagnostic A/B only.  Default behavior is unchanged.  The
     * structured ring trace showed Recomp and xemu are identical through the
     * final common.lin short read, after which this memset is the first known
     * Recomp-only state mutation.  Preserve the pre-existing tail for one
     * bounded comparison run when explicitly requested. */
    if (getenv("RECOMP_SCDA_PRESERVE_SHORT_TAIL")) {
        xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                 "disc short read: diagnostic preserved %u-byte unwritten tail",
                 want > have ? want - have : 0);
        return;
    }

    /* have==0 means this read started already past the file's real end --
     * nothing to zero, let it report EOF/failure normally so a handle
     * that is genuinely exhausted does not turn into an endless supply of
     * fabricated zero chunks (confirmed live, Update 176: lifting this
     * restriction unconditionally produces a request rate that stays
     * linear past 265 phantom chunks with no sign of tapering off -- the
     * title never stops asking on its own, so this is not just "needs more
     * time to converge"). */
    if (!scda_disc_is_tracked(file) || have == 0 || have >= want)
        return;

    /* Tried removing this memset (2026-09-18, fatal-callback investigation),
     * reasoning the destination is the title's own persistent, reused
     * ring-buffer segment so the stale tail should already look like real
     * hardware's untouched RAM. Live-tested and disproven: the very next
     * refill still read a bogus "file handle" argument out of this region
     * either way -- zeroed it reads as handle=0, left alone it reads as
     * whatever stale bytes were there (observed: 0xE1F9F6F3), and both are
     * invalid, so both still fail NtReadFile and arm the fatal callback
     * (sub_0003FCA5). The actual bug is upstream of this function: something
     * sources a "handle" from data-plane memory instead of the object's own
     * persistent field. Keep zeroing -- it is strictly safer (deterministic,
     * no leftover host/guest heap content exposed) and no worse for the
     * still-open boot blocker. See Update 185/186 in the worklog. */
    need = want - have;
    memset(buffer + have, 0, need);
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
             "disc short read: zeroed %u unwritten tail bytes past logical EOF "
             "(reporting the true %u-byte transfer, not topping up)", need, have);
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    WCHAR win_path[MAX_PATH];
    HANDLE h;
    DWORD flags_and_attrs = FILE_ATTRIBUTE_NORMAL;
    (void)AllocationSize;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* A partition device opened as a directory.
     *
     * The path layer maps \Device\Harddisk0\PartitionN to a PartitionN.img
     * backing file, but a title asking for free space opens the partition with
     * FILE_DIRECTORY_FILE | FILE_OPEN_FOR_FREE_SPACE_QUERY -- it wants the
     * volume, not the bytes. Opening a regular file as a directory fails, and
     * the title reads STATUS_OBJECT_PATH_NOT_FOUND as "no such volume".
     *
     * Half-Life 2's CRT probes partition0 this way during startup and treats
     * the failure as fatal. Redirecting to the directory that holds the image
     * gives a handle that is valid for exactly what the caller is going to do
     * with it, which is NtQueryVolumeInformationFile. */
    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) &&
        GetFileAttributesW(win_path) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(win_path) & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR *slash = wcsrchr(win_path, L'\\');
        if (slash && slash != win_path) {
            *slash = 0;
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                     "NtCreateFile: directory open of a device image, "
                     "using its containing directory instead");
        }
    }

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            CreateDirectoryW(win_path, NULL);
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        /* The real NT kernel checks the target's type against the caller's
         * FILE_DIRECTORY_FILE/FILE_NON_DIRECTORY_FILE request before ever
         * opening anything, and answers STATUS_FILE_IS_A_DIRECTORY when a
         * non-directory open lands on a directory. Skipping that check and
         * just calling CreateFileW without FILE_FLAG_BACKUP_SEMANTICS asks
         * Win32 to open a directory as a plain file, which it always
         * refuses with the generic ERROR_ACCESS_DENIED -- a status this
         * title's own error handling does not recognise as benign (it
         * explicitly special-cases STATUS_FILE_IS_A_DIRECTORY, confirmed
         * live: SCDA's directory-existence probes for "Default" and
         * "TDATA\<title-id>" under T:/Z: hit exactly this, and the
         * resulting STATUS_ACCESS_DENIED escaped its error handler and
         * fired the "problem with the disc" fatal path instead of the
         * probe being recognised as "no such directory, try the next
         * device root" that it actually is). Returning the correct status
         * up front avoids the Win32 round-trip entirely and matches real
         * hardware exactly. */
        DWORD attrs = GetFileAttributesW(win_path);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            if (IoStatusBlock) {
                IoStatusBlock->Status = STATUS_FILE_IS_A_DIRECTORY;
                IoStatusBlock->Information = 0;
            }
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (CreateOptions & XBOX_FILE_NO_INTERMEDIATE_BUFFERING)
            flags_and_attrs |= FILE_FLAG_NO_BUFFERING;
        if (FileAttributes & XBOX_FILE_ATTRIBUTE_READONLY)
            flags_and_attrs |= FILE_ATTRIBUTE_READONLY;
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL,
            xbox_disposition_to_win32(CreateDisposition), flags_and_attrs, NULL);
    }

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        /* A warning, not a compiled-out trace: a failed open is how a title
         * silently decides a volume or asset is missing, and in a Release
         * build that decision was invisible. */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtCreateFile FAILED: %S (err=%u)", win_path, err);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        switch (err) {
            case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
            case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
            case ERROR_ACCESS_DENIED:  return STATUS_ACCESS_DENIED;
            case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
            default:                   return STATUS_UNSUCCESSFUL;
        }
    }

    *FileHandle = h;
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %S -> handle=%p", win_path, h);
    scda_umd_track_handle(h, win_path);
    scda_disc_track_handle(h, get_xbox_path(ObjectAttributes));
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_read = 0;
    DWORD io_error = ERROR_SUCCESS;
    BOOL result;
    OVERLAPPED ov;
    int io_timing = scda_io_timing_on();
    LARGE_INTEGER io_begin = {0}, io_prepared = {0}, io_done = {0};
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (io_timing) QueryPerformanceCounter(&io_begin);

    /* GPU surface access tracking may have the buffer protected; ReadFile
     * would fail on it instead of faulting (docs Update 493). */
    xbox_PrepareHostWrite(Buffer, Length);
    if (io_timing) QueryPerformanceCounter(&io_prepared);

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, &ov);
    } else {
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, NULL);
    }
    if (!result) io_error = GetLastError();
    if (io_timing) QueryPerformanceCounter(&io_done);

    if (result || io_error == ERROR_HANDLE_EOF) {
        if (io_timing)
            scda_io_timing_record('R', FileHandle, Length, bytes_read,
                                  io_begin, io_prepared, io_done);
        if (bytes_read < Length) {
            scda_disc_zero_short_read_tail(FileHandle, (unsigned char *)Buffer,
                                            bytes_read, Length);
        }
        IoStatusBlock->Information = bytes_read;
        if (scda_umd_is_tracked(FileHandle))
            scda_skip_intro_patch((unsigned char *)Buffer, bytes_read);
        if (scda_umd_is_tracked(FileHandle)) {
            uint64_t off64 = ByteOffset ? (uint64_t)ByteOffset->QuadPart : 0;
            unsigned char *b = (unsigned char *)Buffer;
            char preview[65];
            unsigned int i, n = bytes_read < 64 ? bytes_read : 64;
            for (i = 0; i < n; i++) {
                unsigned char c = b[i];
                preview[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            preview[n] = '\0';
            fprintf(stderr,
                    "[SCDA-UMD-READ] handle=%p off=0x%08llX%08llX len=%u got=%u \"%s\"\n",
                    FileHandle, (unsigned long long)(off64 >> 32),
                    (unsigned long long)(off64 & 0xffffffffu), Length, bytes_read, preview);
            fflush(stderr);
            /* Full-buffer dump for the three ini member sizes under
             * investigation (2026-09-02 UMOD archive-layer check), so the
             * exact bytes can be diffed against a fresh QuickBMS
             * extraction rather than trusting a 64-byte preview. */
            /* only on request since docs Update 659: RECOMP_UMD_DUMP=1 */
            static int umd_dump = -1;
            if (umd_dump < 0) {
                const char *e = getenv("RECOMP_UMD_DUMP");
                umd_dump = e && e[0] == '1';
            }
            if (umd_dump &&
                (bytes_read == 3526 || bytes_read == 6049 || bytes_read == 24593)) {
                char fname[64];
                FILE *df;
                _snprintf_s(fname, sizeof(fname), _TRUNCATE,
                            "scda_umd_dump_%u.bin", bytes_read);
                if (fopen_s(&df, fname, "wb") == 0 && df) {
                    fwrite(b, 1, bytes_read, df);
                    fclose(df);
                }
            }
        }
        if (bytes_read == 0 && Length > 0) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            return STATUS_END_OF_FILE;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    {
        DWORD last_error = io_error;
        if (io_timing)
            scda_io_timing_record('R', FileHandle, Length, bytes_read,
                                  io_begin, io_prepared, io_done);
        /* Diagnostic only (2026-09-18, fatal-path investigation): XBOX_TRACE
         * is compiled out in RelWithDebInfo, so a hard ReadFile failure here
         * was previously completely invisible in xbox_kernel.log even though
         * it maps to STATUS_UNSUCCESSFUL, a status the guest's own ring-buffer
         * refill code (sub_00040821/sub_000516B7) treats as a genuine fatal
         * I/O error (unlike STATUS_END_OF_FILE, which it special-cases as a
         * clean, non-fatal end of stream). */
        LARGE_INTEGER pos_zero, pos_now;
        pos_zero.QuadPart = 0;
        pos_now.QuadPart = -1;
        SetFilePointerEx(FileHandle, pos_zero, &pos_now, FILE_CURRENT);
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtReadFile(handle=%p, len=%u, bytes_read=%u) failed err=%lu "
                 "buffer=%p byte_offset=%lld file_pos=%lld",
                 FileHandle, Length, bytes_read, last_error, Buffer,
                 ByteOffset ? (long long)ByteOffset->QuadPart : -1LL,
                 (long long)pos_now.QuadPart);
    }
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_written = 0;
    DWORD io_error = ERROR_SUCCESS;
    BOOL result;
    OVERLAPPED ov;
    int io_timing = scda_io_timing_on();
    LARGE_INTEGER io_begin = {0}, io_prepared = {0}, io_done = {0};
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (io_timing) QueryPerformanceCounter(&io_begin);

    /* A buffer inside a GPU surface (e.g. a savegame screenshot) may be
     * read-protected by access tracking (docs Update 493). */
    xbox_PrepareHostRead(Buffer, Length);
    if (io_timing) QueryPerformanceCounter(&io_prepared);

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, &ov);
    } else {
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, NULL);
    }
    if (!result) io_error = GetLastError();
    if (io_timing) QueryPerformanceCounter(&io_done);

    if (result) {
        if (io_timing)
            scda_io_timing_record('W', FileHandle, Length, bytes_written,
                                  io_begin, io_prepared, io_done);
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = bytes_written;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    {
        DWORD last_error = io_error;
        if (io_timing)
            scda_io_timing_record('W', FileHandle, Length, bytes_written,
                                  io_begin, io_prepared, io_done);
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p, len=%u) failed err=%u",
                   FileHandle, Length, last_error);
        (void)last_error; /* XBOX_TRACE is compiled out in release builds. */
    }
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

static void release_dir_context(HANDLE FileHandle);

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        /* A directory enumeration dies with its handle. Left behind, the
         * next directory opened under the same handle value continued the
         * stale scan and answered its first query with NO_MORE_FILES
         * (XFindFirstSaveGame found no saves, docs Update 348). */
        release_dir_context(Handle);
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    WCHAR win_path[MAX_PATH];
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %S", win_path);
    if (DeleteFileW(win_path))    return STATUS_SUCCESS;
    if (RemoveDirectoryW(win_path)) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->AllocationSize.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->AllocationSize.QuadPart + 4095) & ~4095LL;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->NumberOfLinks = fi.nNumberOfLinks;
            info->DeletePending = FALSE;
            info->Directory = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            LARGE_INTEGER pos, zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT))
                return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->EndOfFile.QuadPart + 4095) & ~4095LL;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (!SetFilePointerEx(FileHandle, info->CurrentByteOffset, NULL, FILE_BEGIN)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetFilePosition failed: offset=%lld err=%u",
                    (long long)info->CurrentByteOffset.QuadPart, GetLastError());
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            LARGE_INTEGER cur, zero = {0};
            SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
            SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
            if (!SetEndOfFile(FileHandle)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetEndOfFile failed: size=%lld err=%u",
                    (long long)info->EndOfFile.QuadPart, GetLastError());
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
                return STATUS_UNSUCCESSFUL;
            }
            if (cur.QuadPart <= info->EndOfFile.QuadPart)
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {
            /* Reserve space for a file. Halo's save path calls this before
             * writing, and an unimplemented class here returned
             * STATUS_NOT_IMPLEMENTED, which the title turned into DOS error
             * 317 and reported as "couldn't open or create saved game file".
             *
             * Same payload shape as EndOfFile: one LARGE_INTEGER. Windows
             * FileAllocationInfo is the direct equivalent; if the filesystem
             * declines it, fall back to setting the size, since the caller
             * only needs the space to exist. */
            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            FILE_ALLOCATION_INFO fai;
            fai.AllocationSize = info->EndOfFile;
            if (!SetFileInformationByHandle(FileHandle, FileAllocationInfo,
                                            &fai, sizeof(fai))) {
                LARGE_INTEGER cur, zero = {0};
                SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
                SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
                SetEndOfFile(FileHandle);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            FILE_DISPOSITION_INFO fdi;
            fdi.DeleteFile = info->DeleteFile;
            if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &fdi, sizeof(fdi)))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                         "SetFileDispositionInfo failed: err=%u", GetLastError());
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            FILETIME ct, at, wt;
            ct.dwLowDateTime = info->CreationTime.LowPart;
            ct.dwHighDateTime = info->CreationTime.HighPart;
            at.dwLowDateTime = info->LastAccessTime.LowPart;
            at.dwHighDateTime = info->LastAccessTime.HighPart;
            wt.dwLowDateTime = info->LastWriteTime.LowPart;
            wt.dwHighDateTime = info->LastWriteTime.HighPart;
            SetFileTime(FileHandle, &ct, &at, &wt);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

/* Xbox volume geometry.
 *
 * FATX uses 16 KB clusters: 512-byte sectors, 32 sectors per cluster. That is
 * not cosmetic. A title's CRT startup asks for FileFsSizeInformation and
 * multiplies SectorsPerAllocationUnit by BytesPerSector, then *requires* the
 * product to equal the cluster size it was built for. Half-Life 2 checks for
 * 0x4000 and returns STATUS_DEVICE_NOT_READY (0xC000014F) otherwise, which
 * aborts CRT init before main ever runs -- the process then exits cleanly,
 * which reads as a title that did nothing rather than one that failed.
 *
 * Reporting the host's PC-typical 4 KB cluster (512 x 8) fails that check. */
#define XBOX_BYTES_PER_SECTOR       512u
#define XBOX_SECTORS_PER_CLUSTER    32u      /* 512 * 32 = 16384 */

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)FileHandle; (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                /* Xbox FATX partition1 uses 16 KiB allocation units.  Some
                 * titles validate this geometry during XAPI startup and
                 * reject the host filesystem's native 4 KiB cluster size. */
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = total_bytes.QuadPart / cs;
                info->AvailableAllocationUnits.QuadPart = free_bytes.QuadPart / cs;
            } else {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            /* RECOMP_CACHE_TRACE=1 (docs Update 435): which volume a title
             * sizes and what it is told -- SCDA leaves its mission music out
             * of the Z: cache; a free-space answer derived from the host
             * disk (hundreds of GB) is one candidate. Observational. */
            {
                static int trace = -1;
                static unsigned logged;
                if (trace < 0)
                    trace = getenv("RECOMP_CACHE_TRACE") != NULL;
                if (trace && logged < 200) {
                    char path[MAX_PATH] = "?";
                    logged++;
                    GetFinalPathNameByHandleA(FileHandle, path, sizeof(path), 0);
                    fprintf(stderr, "  [VOLSIZE] %s total_units=%llu avail_units=%llu "
                            "cluster=%lu\n", path,
                            (unsigned long long)info->TotalAllocationUnits.QuadPart,
                            (unsigned long long)info->AvailableAllocationUnits.QuadPart,
                            (unsigned long)(info->BytesPerSector *
                                            info->SectorsPerAllocationUnit));
                }
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    WCHAR win_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (!GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_UNSUCCESSFUL;
    }

    FileInformation->CreationTime.LowPart    = fad.ftCreationTime.dwLowDateTime;
    FileInformation->CreationTime.HighPart   = fad.ftCreationTime.dwHighDateTime;
    FileInformation->LastAccessTime.LowPart  = fad.ftLastAccessTime.dwLowDateTime;
    FileInformation->LastAccessTime.HighPart = fad.ftLastAccessTime.dwHighDateTime;
    FileInformation->LastWriteTime.LowPart   = fad.ftLastWriteTime.dwLowDateTime;
    FileInformation->LastWriteTime.HighPart  = fad.ftLastWriteTime.dwHighDateTime;
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FileInformation->AllocationSize.QuadPart = (FileInformation->EndOfFile.QuadPart + 4095) & ~4095LL;
    FileInformation->FileAttributes = fad.dwFileAttributes;
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE file_handle;
    HANDLE find_handle;
    BOOL   first_done;
    WIN32_FIND_DATAW find_data;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

static void release_dir_context(HANDLE FileHandle)
{
    if (!s_dir_cs_init)
        return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle &&
            s_dir_contexts[i].find_handle != NULL) {
            if (s_dir_contexts[i].find_handle != INVALID_HANDLE_VALUE)
                FindClose(s_dir_contexts[i].find_handle);
            s_dir_contexts[i].find_handle = NULL;
            s_dir_contexts[i].file_handle = NULL;
            s_dir_contexts[i].first_done = FALSE;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

static DIR_CONTEXT* find_or_create_dir_context(HANDLE FileHandle, BOOL create)
{
    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle && s_dir_contexts[i].find_handle != NULL) {
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    if (!create) { LeaveCriticalSection(&s_dir_cs); return NULL; }
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].find_handle == NULL) {
            s_dir_contexts[i].file_handle = FileHandle;
            s_dir_contexts[i].first_done = FALSE;
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    LeaveCriticalSection(&s_dir_cs);
    return NULL;
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    DIR_CONTEXT* ctx;
    PXBOX_FILE_DIRECTORY_INFORMATION entry;
    (void)Event; (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    ctx = find_or_create_dir_context(FileHandle, TRUE);
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (RestartScan || !ctx->first_done) {
        if (ctx->find_handle && ctx->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
        }
        WCHAR search_path[MAX_PATH];
        WCHAR dir_path[MAX_PATH];
        DWORD path_len = GetFinalPathNameByHandleW(FileHandle, dir_path, MAX_PATH,
                                                   FILE_NAME_NORMALIZED);
        if (path_len == 0 || path_len >= MAX_PATH) {
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        WCHAR* clean_path = dir_path;
        if (wcsncmp(clean_path, L"\\\\?\\", 4) == 0)
            clean_path += 4;
        if (FileName && FileName->Buffer) {
            WCHAR pattern_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, FileName->Buffer, FileName->Length,
                                pattern_wide, MAX_PATH);
            pattern_wide[FileName->Length] = L'\0';
            swprintf_s(search_path, MAX_PATH, L"%s\\%s", clean_path, pattern_wide);
        } else {
            swprintf_s(search_path, MAX_PATH, L"%s\\*", clean_path);
        }
        ctx->find_handle = FindFirstFileW(search_path, &ctx->find_data);
        if (ctx->find_handle == INVALID_HANDLE_VALUE) {
            ctx->find_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        ctx->first_done = TRUE;
    } else {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }
    /* FATX has no "." and ".." entries; a title enumerating its save
     * directories (XFindFirstSaveGame) must not see them. */
    while (wcscmp(ctx->find_data.cFileName, L".") == 0 ||
           wcscmp(ctx->find_data.cFileName, L"..") == 0) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);
    char filename_ansi[MAX_PATH];
    int name_len = WideCharToMultiByte(CP_ACP, 0, ctx->find_data.cFileName, -1,
                                       filename_ansi, MAX_PATH, NULL, NULL);
    if (name_len > 0) name_len--;

    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    entry->CreationTime.LowPart   = ctx->find_data.ftCreationTime.dwLowDateTime;
    entry->CreationTime.HighPart  = ctx->find_data.ftCreationTime.dwHighDateTime;
    entry->LastAccessTime.LowPart = ctx->find_data.ftLastAccessTime.dwLowDateTime;
    entry->LastAccessTime.HighPart = ctx->find_data.ftLastAccessTime.dwHighDateTime;
    entry->LastWriteTime.LowPart  = ctx->find_data.ftLastWriteTime.dwLowDateTime;
    entry->LastWriteTime.HighPart = ctx->find_data.ftLastWriteTime.dwHighDateTime;
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = ((LONGLONG)ctx->find_data.nFileSizeHigh << 32) | ctx->find_data.nFileSizeLow;
    entry->AllocationSize.QuadPart = (entry->EndOfFile.QuadPart + 4095) & ~4095LL;
    entry->FileAttributes = ctx->find_data.dwFileAttributes;
    entry->FileNameLength = name_len;
    {
        ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
        if (name_len > 0 && (header_size + name_len) <= Length)
            memcpy(entry->FileName, filename_ansi, name_len);
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = header_size + name_len;
    }
    return STATUS_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  POSIX backend  =================================== */
/* ======================================================================== */

/* Convert Xbox access mask + disposition to POSIX open() flags. */
static int posix_open_flags(ACCESS_MASK access, ULONG disposition)
{
    int wantWrite = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                               XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    int rw = wantWrite ? O_RDWR : O_RDONLY;
    int extra;

    switch (disposition) {
        case XBOX_FILE_SUPERSEDE:    extra = O_CREAT | O_TRUNC; break;
        case XBOX_FILE_OPEN:         extra = 0;                 break;
        case XBOX_FILE_CREATE:       extra = O_CREAT | O_EXCL;  break;
        case XBOX_FILE_OPEN_IF:      extra = O_CREAT;           break;
        case XBOX_FILE_OVERWRITE:    extra = O_TRUNC;           break;
        case XBOX_FILE_OVERWRITE_IF: extra = O_CREAT | O_TRUNC; break;
        default:                     extra = 0;                 break;
    }
    /* O_TRUNC / O_CREAT imply write intent */
    if ((extra & (O_TRUNC | O_CREAT)) && rw == O_RDONLY)
        rw = O_RDWR;
    if (access & XBOX_FILE_APPEND_DATA)
        extra |= O_APPEND;
    return rw | extra;
}

static void unix_to_filetime(time_t sec, long nsec, LARGE_INTEGER* out)
{
    /* 100-ns ticks since 1601-01-01 */
    ULONGLONG t = 116444736000000000ULL
                + (ULONGLONG)sec * 10000000ULL
                + (ULONGLONG)nsec / 100ULL;
    out->LowPart  = (DWORD)(t & 0xFFFFFFFFULL);
    out->HighPart = (LONG)(t >> 32);
}

static ULONG mode_to_xbox_attrs(mode_t m)
{
    ULONG a = 0;
    if (S_ISDIR(m))      a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    if (!(m & S_IWUSR))  a |= XBOX_FILE_ATTRIBUTE_READONLY;
    if (a == 0)          a = XBOX_FILE_ATTRIBUTE_NORMAL;
    return a;
}

static NTSTATUS errno_to_status(int e)
{
    switch (e) {
        case ENOENT:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return STATUS_ACCESS_DENIED;
        case EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
        case ENOMEM:  return STATUS_NO_MEMORY;
        default:      return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    char host_path[MAX_PATH];
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    int fd;
    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            mkdir(host_path, 0755);   /* EEXIST is fine */
        fd = open(host_path, O_RDONLY | O_DIRECTORY);
    } else {
        fd = open(host_path, posix_open_flags(DesiredAccess, CreateDisposition), 0644);
    }

    if (fd < 0) {
        int e = errno;
        XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile FAILED: %s (errno=%d)", host_path, e);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        return errno_to_status(e);
    }

    *FileHandle = w32_open_handle(fd, host_path);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %s -> handle=%p", host_path, *FileHandle);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = read(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Information = (ULONG_PTR)n;
    if (n == 0 && Length > 0) {
        IoStatusBlock->Status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = write(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (ULONG_PTR)n;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char host_path[MAX_PATH];
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %s", host_path);
    if (unlink(host_path) == 0) return STATUS_SUCCESS;
    if (rmdir(host_path)  == 0) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    struct stat st;
    if (FileInformationClass != XboxFilePositionInformation) {
        if (fstat(fd, &st) != 0)
            return STATUS_UNSUCCESSFUL;
    }

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->NumberOfLinks = (ULONG)st.st_nlink;
            info->DeletePending = FALSE;
            info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos < 0) return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset.QuadPart = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (lseek(fd, (off_t)info->CurrentByteOffset.QuadPart, SEEK_SET) < 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            if (ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            /* POSIX: unlinking an open file removes it on last close -- this
             * matches NT "delete on close" semantics exactly. */
            if (info->DeleteFile) {
                const char* p = w32_handle_path(FileHandle);
                if (p) unlink(p);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation:
            /* Setting file times is non-essential for the game; accept it. */
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            struct statvfs vfs;
            int fd = w32_handle_fd(FileHandle);
            /* Xbox geometry, not the host's -- see the note on
             * XBOX_SECTORS_PER_CLUSTER above. */
            info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
            info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
            if (fd >= 0 && fstatvfs(fd, &vfs) == 0) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                ULONGLONG total = (ULONGLONG)vfs.f_blocks * vfs.f_frsize;
                ULONGLONG avail = (ULONGLONG)vfs.f_bavail * vfs.f_frsize;
                info->TotalAllocationUnits.QuadPart = total / cs;
                info->AvailableAllocationUnits.QuadPart = avail / cs;
            } else {
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    int fd = w32_handle_fd(FileHandle);
    if (fd >= 0) fsync(fd);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    char host_path[MAX_PATH];
    struct stat st;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host_path, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_to_filetime(st.st_ctime, 0, &FileInformation->CreationTime);
    unix_to_filetime(st.st_atime, 0, &FileInformation->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &FileInformation->LastWriteTime);
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = st.st_size;
    FileInformation->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    FileInformation->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    return STATUS_SUCCESS;
}

/* Directory enumeration state, keyed by the directory's Nt handle. */
#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE handle;
    DIR*   dir;
    char   pattern[64];
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);

    /* Locate or create the per-handle enumeration context. */
    DIR_CONTEXT* ctx = NULL;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle == FileHandle) { ctx = &s_dir_contexts[i]; break; }
    if (!ctx) {
        for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
            if (s_dir_contexts[i].handle == NULL) { ctx = &s_dir_contexts[i]; break; }
        if (!ctx) { LeaveCriticalSection(&s_dir_cs); return STATUS_INSUFFICIENT_RESOURCES; }
        ctx->handle = FileHandle;
        ctx->dir = NULL;
    }

    if (RestartScan || ctx->dir == NULL) {
        if (ctx->dir) { closedir(ctx->dir); ctx->dir = NULL; }
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) { LeaveCriticalSection(&s_dir_cs); return STATUS_UNSUCCESSFUL; }
        ctx->dir = opendir(dpath);
        if (!ctx->dir) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (FileName && FileName->Buffer && FileName->Length > 0) {
            USHORT n = FileName->Length;
            if (n >= sizeof(ctx->pattern)) n = sizeof(ctx->pattern) - 1;
            memcpy(ctx->pattern, FileName->Buffer, n);
            ctx->pattern[n] = '\0';
        } else {
            strcpy(ctx->pattern, "*");
        }
    }

    /* Advance to the next entry matching the search pattern. */
    struct dirent* de;
    const char* dpath = w32_handle_path(FileHandle);
    struct stat st;
    for (;;) {
        de = readdir(ctx->dir);
        if (!de) {
            closedir(ctx->dir);
            ctx->dir = NULL;
            ctx->handle = NULL;
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0)
            break;
    }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", dpath ? dpath : ".", de->d_name);
    if (stat(full, &st) != 0)
        memset(&st, 0, sizeof(st));
    LeaveCriticalSection(&s_dir_cs);

    PXBOX_FILE_DIRECTORY_INFORMATION entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);

    int name_len = (int)strlen(de->d_name);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    unix_to_filetime(st.st_ctime, 0, &entry->CreationTime);
    unix_to_filetime(st.st_atime, 0, &entry->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &entry->LastWriteTime);
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = st.st_size;
    entry->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    entry->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    entry->FileNameLength = name_len;

    ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    if (name_len > 0 && (header_size + (ULONG)name_len) <= Length)
        memcpy(entry->FileName, de->d_name, name_len);
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = header_size + name_len;
    return STATUS_SUCCESS;
}

#endif /* _WIN32 */

/* ======================================================================== */
/* ====================  Platform-independent  ============================ */
/* ======================================================================== */

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions)
{
    /* NtOpenFile is NtCreateFile with FILE_OPEN disposition */
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, NULL, 0, ShareAccess, XBOX_FILE_OPEN, OpenOptions);
}

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
        AllocationSize, FileAttributes, ShareAccess, Disposition, CreateOptions);
}

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtFsControlFile(0x%X) - stub", FsControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    enum {
        IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x00070000,
        IOCTL_DISK_GET_PARTITION_INFO = 0x00074004,
        XBOX_RAW_PARTITION_LENGTH = 64 * 1024 * 1024
    };
    unsigned char* output = (unsigned char*)OutputBuffer;
    ULONG information = 0;
    uint32_t value32;
    uint64_t value64;

    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength;

    if (!IoStatusBlock || !OutputBuffer)
        return STATUS_INVALID_PARAMETER;

    if (IoControlCode == IOCTL_DISK_GET_DRIVE_GEOMETRY &&
        OutputBufferLength >= 24) {
        /* 32-bit DISK_GEOMETRY: Cylinders, MediaType, TracksPerCylinder,
         * SectorsPerTrack, BytesPerSector. SCDA consumes BytesPerSector; the
         * remaining values describe a consistent 64 MiB fixed disk. */
        memset(output, 0, 24);
        value64 = 64;
        memcpy(output + 0, &value64, sizeof(value64));
        value32 = 12; /* FixedMedia */
        memcpy(output + 8, &value32, sizeof(value32));
        value32 = 32;
        memcpy(output + 12, &value32, sizeof(value32));
        value32 = 64;
        memcpy(output + 16, &value32, sizeof(value32));
        value32 = 512;
        memcpy(output + 20, &value32, sizeof(value32));
        information = 24;
    } else if (IoControlCode == IOCTL_DISK_GET_PARTITION_INFO &&
               OutputBufferLength >= 32) {
        /* 32-bit PARTITION_INFORMATION. The backing file grows only as SCDA
         * writes FATX metadata; this virtual capacity does not reserve or
         * expose a host disk. */
        memset(output, 0, 32);
        value64 = XBOX_RAW_PARTITION_LENGTH;
        memcpy(output + 8, &value64, sizeof(value64));
        value32 = 5;
        memcpy(output + 20, &value32, sizeof(value32));
        output[26] = 1; /* RecognizedPartition */
        information = 32;
    } else {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtDeviceIoControlFile(0x%X) - unsupported", IoControlCode);
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
        return STATUS_NOT_IMPLEMENTED;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = information;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeviceIoControlFile(0x%X) -> %lu bytes",
               IoControlCode, (unsigned long)information);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(
    PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    /*
     * Xbox uses symbolic links for drive-letter mapping (D: -> \Device\CdRom0).
     * Path translation handles this transparently, so return a dummy handle.
     */
    if (LinkHandle)
        *LinkHandle = (HANDLE)(ULONG_PTR)0xDEAD0001;
    XBOX_TRACE(XBOX_LOG_FILE, "NtOpenSymbolicLinkObject(%s) - stub",
        get_xbox_path(ObjectAttributes) ? get_xbox_path(ObjectAttributes) : "?");
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(
    HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    (void)LinkHandle;
    const char* target = "\\Device\\CdRom0";
    if (LinkTarget && LinkTarget->Buffer) {
        USHORT len = (USHORT)strlen(target);
        if (len < LinkTarget->MaximumLength) {
            memcpy(LinkTarget->Buffer, target, len + 1);
            LinkTarget->Length = len;
        }
    }
    if (ReturnedLength)
        *ReturnedLength = (ULONG)strlen(target);
    return STATUS_SUCCESS;
}
