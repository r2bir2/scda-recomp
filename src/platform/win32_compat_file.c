/*
 * win32_compat_file.c - the Win32 file API on POSIX for the SCDA ARM port
 * (docs Update 677).
 *
 * The runtime keeps Windows paths throughout, the way Wine presents them:
 * "Z:\home\user\game" is "/home/user/game" (any drive letter maps to "/"),
 * backslashes separate components and names match case-insensitively. That
 * keeps one tested path layer (kernel_path.c / kernel_file.c take their Win32
 * branch on POSIX too) instead of a second Linux-only one.
 *
 * Linux/POSIX only.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#define W32_NO_FOPEN_MACRO
#include "win32_compat.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#define FT_EPOCH_DIFF 116444736000000000ULL
#define W32_PATH 4096

/* ===================================================================== */
/* Path mapping                                                          */
/* ===================================================================== */

/* Replace each missing component of an absolute or relative host path by a
 * case-insensitive match from its directory; the rest stays as given when a
 * component has no match (a file about to be created). */
static void resolve_ci(char *path, size_t size)
{
    struct stat st;
    char out[W32_PATH];
    size_t olen = 0;
    const char *p = path;

    if (!path[0] || lstat(path, &st) == 0)
        return;
    if (*p == '/') { out[olen++] = '/'; p++; }
    out[olen] = 0;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0) { p++; continue; }
        if (olen && out[olen - 1] != '/' && olen + 1 < sizeof(out))
            out[olen++] = '/';
        const size_t base = olen;
        if (base + n + 1 >= sizeof(out))
            return;
        memcpy(out + base, p, n);
        out[base + n] = 0;
        if (lstat(out, &st) != 0) {
            int found = 0;
            out[base] = 0;
            DIR *d = opendir(base ? out : ".");
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (strlen(de->d_name) == n && !strncasecmp(de->d_name, p, n)) {
                        memcpy(out + base, de->d_name, n);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) {
                /* keep the remainder verbatim */
                snprintf(out + base, sizeof(out) - base, "%s", p);
                snprintf(path, size, "%s", out);
                return;
            }
            out[base + n] = 0;
        }
        olen = base + n;
        p += n;
    }
    snprintf(path, size, "%s", out);
}

char *w32_host_path(const char *win, char *out, size_t size)
{
    const char *s = win;
    size_t o = 0;
    int absolute = 0;

    if (!out || !size) return out;
    out[0] = 0;
    if (!win) return out;
    if ((s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/') &&
        s[2] == '?' && (s[3] == '\\' || s[3] == '/'))
        s += 4;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        s += 2;
        absolute = 1;
    }
    if (absolute && *s != '\\' && *s != '/' && o + 1 < size)
        out[o++] = '/';
    for (; *s && o + 1 < size; s++) {
        char c = (*s == '\\') ? '/' : *s;
        if (c == '/' && o > 0 && out[o - 1] == '/')
            continue;
        out[o++] = c;
    }
    out[o] = 0;
    resolve_ci(out, size);
    return out;
}

char *w32_win_path(const char *host, char *out, size_t size)
{
    size_t o = 0;
    if (!out || !size) return out;
    if (host[0] == '/' && size > 3) { out[o++] = 'Z'; out[o++] = ':'; }
    for (; *host && o + 1 < size; host++)
        out[o++] = (*host == '/') ? '\\' : *host;
    out[o] = 0;
    return out;
}

static void w_to_utf8(LPCWSTR w, char *out, size_t size)
{
    out[0] = 0;
    if (w && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL) <= 0)
        out[0] = 0;
}

/* copy a UTF-8 result into a W buffer of n chars, Win32 return convention:
 * length without the terminator when it fits, else the size needed */
static DWORD utf8_to_w_ret(const char *s, LPWSTR buf, DWORD n)
{
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= 0) return 0;
    if (!buf || (DWORD)need > n) return (DWORD)need;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, (int)n);
    return (DWORD)need - 1;
}

static DWORD utf8_ret(const char *s, LPSTR buf, DWORD n)
{
    size_t len = strlen(s);
    if (!buf || len + 1 > n) return (DWORD)len + 1;
    memcpy(buf, s, len + 1);
    return (DWORD)len;
}

static DWORD errno_to_win32(int e)
{
    switch (e) {
    case 0:         return ERROR_SUCCESS;
    case ENOENT:    return ERROR_FILE_NOT_FOUND;
    case ENOTDIR:   return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR:    return ERROR_ACCESS_DENIED;
    case EEXIST:    return ERROR_FILE_EXISTS;
    case ENOSPC:    return ERROR_DISK_FULL;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EBADF:     return ERROR_INVALID_HANDLE;
    case EINVAL:    return ERROR_INVALID_PARAMETER;
    case ENAMETOOLONG: return ERROR_BAD_PATHNAME;
    case ENOMEM:    return ERROR_NOT_ENOUGH_MEMORY;
    default:        return ERROR_GEN_FAILURE;
    }
}

/* ENOENT is ERROR_PATH_NOT_FOUND when the parent directory is missing too */
static void set_path_error(const char *host, int e)
{
    if (e == ENOENT) {
        char parent[W32_PATH];
        struct stat st;
        snprintf(parent, sizeof(parent), "%s", host);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0;
            if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) {
                SetLastError(ERROR_PATH_NOT_FOUND);
                return;
            }
        }
    }
    SetLastError(errno_to_win32(e));
}

FILE *w32_fopen(const char *path, const char *mode)
{
    char host[W32_PATH];
    return fopen(w32_host_path(path, host, sizeof(host)), mode);
}

/* ===================================================================== */
/* Times and attributes                                                  */
/* ===================================================================== */

static FILETIME ts_to_ft(struct timespec ts)
{
    ULONGLONG t = FT_EPOCH_DIFF + (ULONGLONG)ts.tv_sec * 10000000ULL +
                  (ULONGLONG)ts.tv_nsec / 100ULL;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    return ft;
}

static struct timespec ft_to_ts(const FILETIME *ft)
{
    struct timespec ts;
    ULONGLONG t = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (t < FT_EPOCH_DIFF) t = FT_EPOCH_DIFF;
    t -= FT_EPOCH_DIFF;
    ts.tv_sec = (time_t)(t / 10000000ULL);
    ts.tv_nsec = (long)(t % 10000000ULL) * 100;
    return ts;
}

static DWORD st_attrs(const struct stat *st)
{
    DWORD a = 0;
    if (S_ISDIR(st->st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

static void st_fill_attr_data(const struct stat *st, WIN32_FILE_ATTRIBUTE_DATA *d)
{
    d->dwFileAttributes = st_attrs(st);
    d->ftCreationTime = ts_to_ft(st->st_mtim);
    d->ftLastAccessTime = ts_to_ft(st->st_atim);
    d->ftLastWriteTime = ts_to_ft(st->st_mtim);
    d->nFileSizeHigh = S_ISDIR(st->st_mode) ? 0 : (DWORD)((uint64_t)st->st_size >> 32);
    d->nFileSizeLow = S_ISDIR(st->st_mode) ? 0 : (DWORD)st->st_size;
}

DWORD GetFileAttributesA(LPCSTR path)
{
    char host[W32_PATH];
    struct stat st;
    if (stat(w32_host_path(path, host, sizeof(host)), &st) != 0) {
        set_path_error(host, errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    return st_attrs(&st);
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return GetFileAttributesA(p);
}

BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID info)
{
    char host[W32_PATH];
    struct stat st;
    if (lvl != GetFileExInfoStandard || !info) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (stat(w32_host_path(path, host, sizeof(host)), &st) != 0) {
        set_path_error(host, errno);
        return FALSE;
    }
    st_fill_attr_data(&st, (WIN32_FILE_ATTRIBUTE_DATA *)info);
    return TRUE;
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID info)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return GetFileAttributesExA(p, lvl, info);
}

BOOL SetFileAttributesA(LPCSTR path, DWORD attr)
{
    char host[W32_PATH];
    struct stat st;
    if (stat(w32_host_path(path, host, sizeof(host)), &st) != 0) {
        set_path_error(host, errno);
        return FALSE;
    }
    mode_t m = (attr & FILE_ATTRIBUTE_READONLY) ? (st.st_mode & ~(mode_t)0222)
                                                : (st.st_mode | S_IWUSR);
    if (m != st.st_mode && chmod(host, m & 07777) != 0) {
        SetLastError(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL SetFileAttributesW(LPCWSTR path, DWORD attr)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return SetFileAttributesA(p, attr);
}

/* ===================================================================== */
/* Open, read, write                                                     */
/* ===================================================================== */

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    char host[W32_PATH];
    struct stat st;
    (void)share; (void)sa; (void)templ;

    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    w32_host_path(name, host, sizeof(host));
    const int existed = stat(host, &st) == 0;
    const int is_dir = existed && S_ISDIR(st.st_mode);

    const int want_w = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
                                  FILE_APPEND_DATA)) != 0;
    const int want_r = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    int oflags = want_w ? (want_r ? O_RDWR : O_WRONLY) : O_RDONLY;

    switch (disp) {
    case CREATE_NEW:        oflags |= O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS:       oflags |= O_CREAT;           break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC;           break;
    default:                                             break;
    }
    if ((oflags & (O_CREAT | O_TRUNC)) && (oflags & O_ACCMODE) == O_RDONLY)
        oflags = (oflags & ~O_ACCMODE) | O_RDWR;

    if (is_dir) {
        /* directories open only with backup semantics, and only for reading */
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) || disp == CREATE_NEW ||
            disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING) {
            SetLastError(disp == CREATE_NEW ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        oflags = O_RDONLY | O_DIRECTORY;
    }

    int fd = open(host, oflags | O_CLOEXEC, 0644);
    if (fd < 0 && errno == EACCES && (oflags & O_ACCMODE) != O_RDONLY && !want_w) {
        oflags &= ~(O_ACCMODE | O_TRUNC);
        fd = open(host, oflags | O_RDONLY | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        set_path_error(host, errno);
        return INVALID_HANDLE_VALUE;
    }
    if (flags & FILE_FLAG_DELETE_ON_CLOSE)
        unlink(host);

    char abs_path[PATH_MAX];
    const char *stored = realpath(host, abs_path) ? abs_path : host;
    /* OPEN_ALWAYS/CREATE_ALWAYS report an existing file through the
     * last error while succeeding */
    SetLastError((existed && (disp == OPEN_ALWAYS || disp == CREATE_ALWAYS))
                 ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return w32_open_handle(fd, stored);
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    char p[W32_PATH];
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    w_to_utf8(name, p, sizeof(p));
    return CreateFileA(p, access, share, sa, disp, flags, templ);
}

static int handle_fd(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) SetLastError(ERROR_INVALID_HANDLE);
    return fd;
}

/* A synchronous handle with an OVERLAPPED transfers at its offset and leaves
 * the file pointer after the data; past the end that is ERROR_HANDLE_EOF. */
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, LPOVERLAPPED ov)
{
    int fd = handle_fd(h);
    ssize_t n;
    if (nread) *nread = 0;
    if (fd < 0) return FALSE;
    if (ov) {
        off_t off = (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset);
        size_t done = 0;
        do {
            n = pread(fd, (char *)buf + done, len - done, off + (off_t)done);
        } while ((n > 0 && (done += (size_t)n) < len) || (n < 0 && errno == EINTR));
        if (n < 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
        lseek(fd, off + (off_t)done, SEEK_SET);
        ov->Internal = 0;
        ov->InternalHigh = done;
        if (ov->hEvent) SetEvent(ov->hEvent);
        if (nread) *nread = (DWORD)done;
        if (done == 0 && len) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        return TRUE;
    }
    size_t done = 0;
    do {
        n = read(fd, (char *)buf + done, len - done);
    } while ((n > 0 && (done += (size_t)n) < len) || (n < 0 && errno == EINTR));
    if (n < 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
    if (nread) *nread = (DWORD)done;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, LPOVERLAPPED ov)
{
    int fd = handle_fd(h);
    ssize_t n;
    size_t done = 0;
    if (nwritten) *nwritten = 0;
    if (fd < 0) return FALSE;
    if (ov) {
        off_t off = (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset);
        if (ov->Offset == 0xFFFFFFFFu && ov->OffsetHigh == 0xFFFFFFFFu)
            off = lseek(fd, 0, SEEK_END);
        do {
            n = pwrite(fd, (const char *)buf + done, len - done, off + (off_t)done);
        } while ((n > 0 && (done += (size_t)n) < len) || (n < 0 && errno == EINTR));
        if (n < 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
        lseek(fd, off + (off_t)done, SEEK_SET);
        ov->Internal = 0;
        ov->InternalHigh = done;
        if (ov->hEvent) SetEvent(ov->hEvent);
    } else {
        do {
            n = write(fd, (const char *)buf + done, len - done);
        } while ((n > 0 && (done += (size_t)n) < len) || (n < 0 && errno == EINTR));
        if (n < 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
    }
    if (nwritten) *nwritten = (DWORD)done;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    struct stat st;
    int fd = handle_fd(h);
    if (fd < 0 || fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)((uint64_t)st.st_size >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)st.st_size;
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    struct stat st;
    int fd = handle_fd(h);
    if (fd < 0) return FALSE;
    if (fstat(fd, &st) != 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = handle_fd(h);
    if (fd < 0) return FALSE;
    if (fdatasync(fd) != 0 && errno != EINVAL) {
        SetLastError(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method)
{
    int fd = handle_fd(h);
    int whence = method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET;
    if (fd < 0) return FALSE;
    off_t r = lseek(fd, (off_t)dist.QuadPart, whence);
    if (r < 0) {
        SetLastError(errno == EINVAL ? ERROR_NEGATIVE_SEEK : errno_to_win32(errno));
        return FALSE;
    }
    if (newpos) newpos->QuadPart = (LONGLONG)r;
    return TRUE;
}

DWORD SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method)
{
    LARGE_INTEGER d, r;
    d.QuadPart = hi ? (LONGLONG)(((uint64_t)(uint32_t)*hi << 32) | (uint32_t)lo) : (LONGLONG)lo;
    if (!SetFilePointerEx(h, d, &r, method))
        return INVALID_SET_FILE_POINTER;
    if (hi) *hi = (LONG)(r.QuadPart >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)r.QuadPart;
}

BOOL SetEndOfFile(HANDLE h)
{
    int fd = handle_fd(h);
    if (fd < 0) return FALSE;
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(fd, pos) != 0) {
        SetLastError(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    struct stat st;
    WIN32_FILE_ATTRIBUTE_DATA d;
    int fd = handle_fd(h);
    if (fd < 0) return FALSE;
    if (fstat(fd, &st) != 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
    st_fill_attr_data(&st, &d);
    info->dwFileAttributes = d.dwFileAttributes;
    info->ftCreationTime = d.ftCreationTime;
    info->ftLastAccessTime = d.ftLastAccessTime;
    info->ftLastWriteTime = d.ftLastWriteTime;
    info->dwVolumeSerialNumber = (DWORD)st.st_dev;
    info->nFileSizeHigh = d.nFileSizeHigh;
    info->nFileSizeLow = d.nFileSizeLow;
    info->nNumberOfLinks = (DWORD)st.st_nlink;
    info->nFileIndexHigh = (DWORD)((uint64_t)st.st_ino >> 32);
    info->nFileIndexLow = (DWORD)st.st_ino;
    return TRUE;
}

BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w)
{
    struct timespec ts[2];
    int fd = handle_fd(h);
    (void)c;
    if (fd < 0) return FALSE;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_nsec = UTIME_OMIT;
    if (a && (a->dwLowDateTime | a->dwHighDateTime)) ts[0] = ft_to_ts(a);
    if (w && (w->dwLowDateTime | w->dwHighDateTime)) ts[1] = ft_to_ts(w);
    if (futimens(fd, ts) != 0) { SetLastError(errno_to_win32(errno)); return FALSE; }
    return TRUE;
}

BOOL SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls,
                                LPVOID info, DWORD size)
{
    int fd = handle_fd(h);
    (void)size;
    if (fd < 0) return FALSE;
    switch (cls) {
    case FileDispositionInfo: {
        /* POSIX may unlink an open file, so the delete happens now rather
         * than at the last close; the open handle keeps working */
        const char *path = w32_handle_path(h);
        struct stat st;
        if (!((FILE_DISPOSITION_INFO *)info)->DeleteFile)
            return TRUE;
        if (!path || fstat(fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
        if ((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0 && errno != ENOENT) {
            SetLastError(errno_to_win32(errno));
            return FALSE;
        }
        return TRUE;
    }
    case FileAllocationInfo:
        return TRUE;   /* a hint */
    case FileEndOfFileInfo:
        if (ftruncate(fd, (off_t)((FILE_END_OF_FILE_INFO *)info)->EndOfFile.QuadPart) != 0) {
            SetLastError(errno_to_win32(errno));
            return FALSE;
        }
        return TRUE;
    case FileBasicInfo: {
        const FILE_BASIC_INFO *b = (const FILE_BASIC_INFO *)info;
        FILETIME a = { (DWORD)b->LastAccessTime.QuadPart, (DWORD)(b->LastAccessTime.QuadPart >> 32) };
        FILETIME w = { (DWORD)b->LastWriteTime.QuadPart, (DWORD)(b->LastWriteTime.QuadPart >> 32) };
        return SetFileTime(h, NULL, &a, &w);
    }
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}

DWORD GetFinalPathNameByHandleA(HANDLE h, LPSTR buf, DWORD n, DWORD flags)
{
    char win[W32_PATH + 8];
    const char *path = w32_handle_path(h);
    (void)flags;
    if (!path) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    memcpy(win, "\\\\?\\", 4);
    w32_win_path(path, win + 4, sizeof(win) - 4);
    return utf8_ret(win, buf, n);
}

DWORD GetFinalPathNameByHandleW(HANDLE h, LPWSTR buf, DWORD n, DWORD flags)
{
    char win[W32_PATH + 8];
    const char *path = w32_handle_path(h);
    (void)flags;
    if (!path) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    memcpy(win, "\\\\?\\", 4);
    w32_win_path(path, win + 4, sizeof(win) - 4);
    return utf8_to_w_ret(win, buf, n);
}

BOOL CancelIo(HANDLE h)
{
    (void)h;
    return TRUE;   /* all I/O here is synchronous */
}

BOOL DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_size, LPVOID out,
                     DWORD out_size, LPDWORD returned, LPOVERLAPPED ov)
{
    (void)in; (void)in_size; (void)out; (void)out_size; (void)ov;
    if (returned) *returned = 0;
    if (code == FSCTL_SET_SPARSE && w32_handle_fd(h) >= 0)
        return TRUE;   /* Linux files are sparse already */
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* ===================================================================== */
/* Directory enumeration                                                 */
/* ===================================================================== */

#define W32_FIND_MAGIC 0x46494E44u   /* "FIND" */

typedef struct {
    uint32_t magic;
    DIR *dir;
    char dir_path[W32_PATH];
    char pattern[NAME_MAX + 1];
} w32_find;

/* next entry matching the pattern into a/w (one of them non-NULL) */
static BOOL find_next(w32_find *f, LPWIN32_FIND_DATAA a, LPWIN32_FIND_DATAW w)
{
    struct dirent *de;
    while ((de = readdir(f->dir)) != NULL) {
        if (fnmatch(f->pattern, de->d_name, FNM_CASEFOLD) != 0)
            continue;
        char full[W32_PATH + NAME_MAX + 2];
        struct stat st;
        WIN32_FILE_ATTRIBUTE_DATA d;
        snprintf(full, sizeof(full), "%s/%s", f->dir_path, de->d_name);
        if (stat(full, &st) != 0)
            continue;
        st_fill_attr_data(&st, &d);
        if (a) {
            memset(a, 0, sizeof(*a));
            a->dwFileAttributes = d.dwFileAttributes;
            a->ftCreationTime = d.ftCreationTime;
            a->ftLastAccessTime = d.ftLastAccessTime;
            a->ftLastWriteTime = d.ftLastWriteTime;
            a->nFileSizeHigh = d.nFileSizeHigh;
            a->nFileSizeLow = d.nFileSizeLow;
            snprintf(a->cFileName, sizeof(a->cFileName), "%s", de->d_name);
        } else {
            memset(w, 0, sizeof(*w));
            w->dwFileAttributes = d.dwFileAttributes;
            w->ftCreationTime = d.ftCreationTime;
            w->ftLastAccessTime = d.ftLastAccessTime;
            w->ftLastWriteTime = d.ftLastWriteTime;
            w->nFileSizeHigh = d.nFileSizeHigh;
            w->nFileSizeLow = d.nFileSizeLow;
            if (MultiByteToWideChar(CP_UTF8, 0, de->d_name, -1, w->cFileName, MAX_PATH) <= 0)
                continue;
        }
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

static HANDLE find_first(LPCSTR pattern, LPWIN32_FIND_DATAA a, LPWIN32_FIND_DATAW w)
{
    char path[W32_PATH];
    w32_find *f;
    if (!pattern) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    /* split the pattern off before mapping, it must not be case-resolved */
    snprintf(path, sizeof(path), "%s", pattern);
    char *sep = strrchr(path, '\\');
    char *sep2 = strrchr(path, '/');
    if (!sep || (sep2 && sep2 > sep)) sep = sep2;
    f = (w32_find *)calloc(1, sizeof(*f));
    f->magic = W32_FIND_MAGIC;
    if (sep) {
        snprintf(f->pattern, sizeof(f->pattern), "%s", sep + 1);
        sep[1] = 0;
        w32_host_path(path, f->dir_path, sizeof(f->dir_path));
    } else {
        snprintf(f->pattern, sizeof(f->pattern), "%s", path);
        snprintf(f->dir_path, sizeof(f->dir_path), ".");
    }
    if (!strcmp(f->pattern, "*.*") || !f->pattern[0])
        snprintf(f->pattern, sizeof(f->pattern), "*");
    f->dir = opendir(f->dir_path);
    if (!f->dir) {
        set_path_error(f->dir_path, errno);
        if (GetLastError() == ERROR_FILE_NOT_FOUND) SetLastError(ERROR_PATH_NOT_FOUND);
        free(f);
        return INVALID_HANDLE_VALUE;
    }
    if (!find_next(f, a, w)) {
        closedir(f->dir);
        free(f);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)f;
}

HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
    return find_first(pattern, data, NULL);
}

HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data)
{
    char p[W32_PATH];
    w_to_utf8(pattern, p, sizeof(p));
    return find_first(p, NULL, data);
}

static w32_find *find_handle(HANDLE h)
{
    w32_find *f = (w32_find *)h;
    if (!f || h == INVALID_HANDLE_VALUE || f->magic != W32_FIND_MAGIC) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return f;
}

BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
    w32_find *f = find_handle(h);
    return f ? find_next(f, data, NULL) : FALSE;
}

BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data)
{
    w32_find *f = find_handle(h);
    return f ? find_next(f, NULL, data) : FALSE;
}

BOOL FindClose(HANDLE h)
{
    w32_find *f = find_handle(h);
    if (!f) return FALSE;
    closedir(f->dir);
    f->magic = 0;
    free(f);
    return TRUE;
}

/* ===================================================================== */
/* Directories, delete, move, copy                                       */
/* ===================================================================== */

BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    char host[W32_PATH];
    (void)sa;
    if (mkdir(w32_host_path(path, host, sizeof(host)), 0755) != 0) {
        if (errno == EEXIST) SetLastError(ERROR_ALREADY_EXISTS);
        else if (errno == ENOENT) SetLastError(ERROR_PATH_NOT_FOUND);
        else SetLastError(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return CreateDirectoryA(p, sa);
}

BOOL RemoveDirectoryA(LPCSTR path)
{
    char host[W32_PATH];
    if (rmdir(w32_host_path(path, host, sizeof(host))) != 0) {
        set_path_error(host, errno);
        return FALSE;
    }
    return TRUE;
}

BOOL RemoveDirectoryW(LPCWSTR path)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return RemoveDirectoryA(p);
}

BOOL DeleteFileA(LPCSTR path)
{
    char host[W32_PATH];
    struct stat st;
    w32_host_path(path, host, sizeof(host));
    if (stat(host, &st) == 0 && S_ISDIR(st.st_mode)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (unlink(host) != 0) {
        set_path_error(host, errno);
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR path)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return DeleteFileA(p);
}

static BOOL copy_file(const char *from, const char *to, int excl)
{
    char buf[65536];
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) { set_path_error(from, errno); return FALSE; }
    int out = open(to, O_WRONLY | O_CREAT | O_CLOEXEC | (excl ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) {
        SetLastError(errno == EEXIST ? ERROR_FILE_EXISTS : errno_to_win32(errno));
        close(in);
        return FALSE;
    }
    ssize_t n;
    BOOL ok = TRUE;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) { ok = FALSE; break; }
    }
    if (n < 0) ok = FALSE;
    if (!ok) SetLastError(errno_to_win32(errno));
    close(in);
    close(out);
    return ok;
}

BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    char hf[W32_PATH], ht[W32_PATH];
    return copy_file(w32_host_path(from, hf, sizeof(hf)),
                     w32_host_path(to, ht, sizeof(ht)), fail_if_exists);
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    char f[W32_PATH], t[W32_PATH];
    w_to_utf8(from, f, sizeof(f));
    w_to_utf8(to, t, sizeof(t));
    return CopyFileA(f, t, fail_if_exists);
}

BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    char hf[W32_PATH], ht[W32_PATH];
    struct stat st;
    w32_host_path(from, hf, sizeof(hf));
    w32_host_path(to, ht, sizeof(ht));
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(ht, &st) == 0) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(hf, ht) == 0)
        return TRUE;
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
        if (!copy_file(hf, ht, 0)) return FALSE;
        unlink(hf);
        return TRUE;
    }
    set_path_error(hf, errno);
    return FALSE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    char f[W32_PATH], t[W32_PATH];
    w_to_utf8(from, f, sizeof(f));
    w_to_utf8(to, t, sizeof(t));
    return MoveFileExA(f, t, flags);
}

BOOL MoveFileA(LPCSTR from, LPCSTR to)
{
    return MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED);
}

/* SHCreateDirectoryEx: the whole chain; returns a Win32 error code */
int SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, LPSECURITY_ATTRIBUTES sa)
{
    char p[W32_PATH], host[W32_PATH];
    struct stat st;
    (void)hwnd; (void)sa;
    w_to_utf8(path, p, sizeof(p));
    w32_host_path(p, host, sizeof(host));
    if (stat(host, &st) == 0)
        return S_ISDIR(st.st_mode) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
    for (char *c = host + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            if (mkdir(host, 0755) != 0 && errno != EEXIST) return (int)errno_to_win32(errno);
            *c = '/';
        }
    }
    if (mkdir(host, 0755) != 0 && errno != EEXIST) return (int)errno_to_win32(errno);
    return ERROR_SUCCESS;
}

/* ===================================================================== */
/* Process paths and volumes                                             */
/* ===================================================================== */

static void module_path(char *win, size_t size)
{
    char host[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", host, sizeof(host) - 1);
    host[n > 0 ? n : 0] = 0;
    w32_win_path(host, win, size);
}

/* GetModuleFileName truncates and terminates when the buffer is short */
DWORD GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD n)
{
    char win[W32_PATH];
    (void)mod;
    if (!buf || !n) return 0;
    module_path(win, sizeof(win));
    size_t len = strlen(win);
    if (len + 1 > n) {
        memcpy(buf, win, n - 1);
        buf[n - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(buf, win, len + 1);
    return (DWORD)len;
}

DWORD GetModuleFileNameW(HMODULE mod, LPWSTR buf, DWORD n)
{
    char win[W32_PATH];
    (void)mod;
    if (!buf || !n) return 0;
    module_path(win, sizeof(win));
    DWORD r = utf8_to_w_ret(win, buf, n);
    if (r >= n) {
        MultiByteToWideChar(CP_UTF8, 0, win, (int)n - 1, buf, (int)n);
        buf[n - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    return r;
}

static void cwd_win(char *win, size_t size)
{
    char host[PATH_MAX];
    if (!getcwd(host, sizeof(host))) snprintf(host, sizeof(host), "/");
    w32_win_path(host, win, size);
}

DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf)
{
    char win[W32_PATH];
    cwd_win(win, sizeof(win));
    return utf8_ret(win, buf, n);
}

DWORD GetCurrentDirectoryW(DWORD n, LPWSTR buf)
{
    char win[W32_PATH];
    cwd_win(win, sizeof(win));
    return utf8_to_w_ret(win, buf, n);
}

BOOL SetCurrentDirectoryA(LPCSTR path)
{
    char host[W32_PATH];
    if (chdir(w32_host_path(path, host, sizeof(host))) != 0) {
        set_path_error(host, errno);
        return FALSE;
    }
    return TRUE;
}

BOOL SetCurrentDirectoryW(LPCWSTR path)
{
    char p[W32_PATH];
    w_to_utf8(path, p, sizeof(p));
    return SetCurrentDirectoryA(p);
}

DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR buf, LPSTR *file_part)
{
    char win[W32_PATH];
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if ((isalpha((unsigned char)path[0]) && path[1] == ':') || path[0] == '\\' || path[0] == '/') {
        if (path[0] == '\\' || path[0] == '/')
            snprintf(win, sizeof(win), "Z:%s", path);
        else
            snprintf(win, sizeof(win), "%s", path);
    } else {
        char cwd[W32_PATH];
        cwd_win(cwd, sizeof(cwd));
        snprintf(win, sizeof(win), "%s\\%s", cwd, path);
    }
    for (char *c = win; *c; c++) if (*c == '/') *c = '\\';
    DWORD r = utf8_ret(win, buf, n);
    if (file_part && buf && r < n) {
        char *s = strrchr(buf, '\\');
        *file_part = (s && s[1]) ? s + 1 : NULL;
    }
    return r;
}

BOOL GetDiskFreeSpaceExA(LPCSTR path, PULARGE_INTEGER avail, PULARGE_INTEGER total,
                         PULARGE_INTEGER free_bytes)
{
    char host[W32_PATH];
    struct statvfs v;
    if (path) w32_host_path(path, host, sizeof(host));
    else snprintf(host, sizeof(host), ".");
    if (statvfs(host, &v) != 0) { set_path_error(host, errno); return FALSE; }
    if (avail) avail->QuadPart = (ULONGLONG)v.f_bavail * v.f_frsize;
    if (total) total->QuadPart = (ULONGLONG)v.f_blocks * v.f_frsize;
    if (free_bytes) free_bytes->QuadPart = (ULONGLONG)v.f_bfree * v.f_frsize;
    return TRUE;
}

BOOL GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER avail, PULARGE_INTEGER total,
                         PULARGE_INTEGER free_bytes)
{
    char p[W32_PATH];
    if (!path) return GetDiskFreeSpaceExA(NULL, avail, total, free_bytes);
    w_to_utf8(path, p, sizeof(p));
    return GetDiskFreeSpaceExA(p, avail, total, free_bytes);
}

/* The XDG equivalents: local app data is ~/.local/share, roaming ~/.config. */
static BOOL folder_path(int csidl, char *win, size_t size)
{
    char host[PATH_MAX];
    const char *home = getenv("HOME");
    const char *xdg;
    if (!home || !home[0]) home = "/tmp";
    switch (csidl & 0xFF) {
    case CSIDL_LOCAL_APPDATA:
        xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) snprintf(host, sizeof(host), "%s", xdg);
        else snprintf(host, sizeof(host), "%s/.local/share", home);
        break;
    case CSIDL_APPDATA:
        xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0]) snprintf(host, sizeof(host), "%s", xdg);
        else snprintf(host, sizeof(host), "%s/.config", home);
        break;
    case CSIDL_PERSONAL:
        snprintf(host, sizeof(host), "%s", home);
        break;
    default:
        return FALSE;
    }
    /* like Windows, the folder exists once asked for */
    for (char *c = host + 1; *c; c++) {
        if (*c == '/') { *c = 0; mkdir(host, 0755); *c = '/'; }
    }
    mkdir(host, 0755);
    w32_win_path(host, win, size);
    return TRUE;
}

HRESULT SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path)
{
    char win[W32_PATH];
    (void)hwnd; (void)token; (void)flags;
    if (!path || !folder_path(csidl, win, sizeof(win))) return E_FAIL;
    snprintf(path, MAX_PATH, "%s", win);
    return S_OK;
}

HRESULT SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path)
{
    char win[W32_PATH];
    (void)hwnd; (void)token; (void)flags;
    if (!path || !folder_path(csidl, win, sizeof(win))) return E_FAIL;
    if (MultiByteToWideChar(CP_UTF8, 0, win, -1, path, MAX_PATH) <= 0) return E_FAIL;
    return S_OK;
}

#endif /* !_WIN32 */
