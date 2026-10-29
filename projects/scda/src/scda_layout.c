/*
 * scda_layout.c - release folder layout (docs Update 659), see scda_layout.h.
 *
 * gamefiles\ may hold an Xbox ISO instead of the unpacked game. The runtime
 * opens game files by host path in many places (kernel file layer, cache
 * copies, FMV player, directory listings), so instead of a virtual file
 * system the ISO is unpacked once, next to itself, into gamefiles\<name>\.
 * A marker file written last tells a finished unpack from an interrupted one;
 * the ISO itself is only read.
 *
 * XDVDFS: the volume descriptor sits in sector 32 of the game partition
 * ("MICROSOFT*XBOX*MEDIA", root directory sector and size). A directory is a
 * binary tree of entries: u16 left, u16 right (offsets in dwords, 0 = none),
 * u32 sector, u32 size, u8 attributes (0x10 = directory), u8 name length,
 * name. A trimmed XISO starts with the game partition; full disc images
 * carry a video partition in front of it.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "scda_layout.h"

#define SCDA_TITLE_ID       0x5553005Eu
#define SCDA_XBE_TIMESTAMP  0x44EB67D5u   /* the XBE this build was made from */
#define EXTRACT_MARKER      ".scda_unpacked"
#define XDVD_SECTOR         2048u
#define XDVD_MAGIC          "MICROSOFT*XBOX*MEDIA"
#define COPY_CHUNK          (1u << 20)

static const char s_caption[] = "SCDA Recomp";

static int is_file(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void fail(const char *text)
{
    MessageBoxA(NULL, text, s_caption, MB_OK | MB_ICONERROR | MB_TOPMOST);
    ExitProcess(1);
}

/* ---- game check ------------------------------------------------------- */

static int xbe_identity(const char *path, uint32_t *title_id, uint32_t *stamp)
{
    unsigned char h[0x1000];
    FILE *f = NULL;
    size_t n;
    uint32_t base, cert, off;

    if (fopen_s(&f, path, "rb") != 0 || !f)
        return 0;
    n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 0x120 || memcmp(h, "XBEH", 4) != 0)
        return 0;
    base = *(uint32_t *)(h + 0x104);
    *stamp = *(uint32_t *)(h + 0x114);
    cert = *(uint32_t *)(h + 0x118);
    off = cert - base;
    if (cert < base || off + 12 > n)
        return 0;
    *title_id = *(uint32_t *)(h + off + 8);
    return 1;
}

/* The recompiled code belongs to one exact XBE; another version or region
 * has different code and data and will crash. */
static void check_game(const char *game_dir)
{
    char xbe[MAX_PATH], text[1024];
    uint32_t title_id = 0, stamp = 0;

    _snprintf_s(xbe, sizeof(xbe), _TRUNCATE, "%s\\default.xbe", game_dir);
    if (!xbe_identity(xbe, &title_id, &stamp)) {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "default.xbe could not be read or is not an Xbox "
                    "executable:\n\n%s", xbe);
        fail(text);
    }
    if (title_id != SCDA_TITLE_ID) {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "The game in the \"gamefiles\" folder is not Splinter "
                    "Cell: Double Agent (title ID %08X, expected %08X):\n\n%s",
                    title_id, SCDA_TITLE_ID, game_dir);
        fail(text);
    }
    if (stamp != SCDA_XBE_TIMESTAMP) {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "This copy of Splinter Cell: Double Agent is a different "
                    "version than the one this recompilation was made from "
                    "(XBE build %08X, expected %08X: the USA/Europe disc, "
                    "serial US-094).\n\n"
                    "It will most likely not work. Start anyway?",
                    stamp, SCDA_XBE_TIMESTAMP);
        if (MessageBoxA(NULL, text, s_caption,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                        MB_TOPMOST) != IDYES)
            ExitProcess(1);
    }
}

/* ---- progress window -------------------------------------------------- */

static HWND s_wnd;
static volatile int s_cancel;
static char s_line1[160], s_line2[MAX_PATH];
static int s_permille;

#if defined(_WIN32)
static LRESULT CALLBACK progress_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) {
        if (MessageBoxA(h, "Stop unpacking the ISO?\n\nIt starts over on the "
                        "next launch.", s_caption,
                        MB_YESNO | MB_ICONQUESTION) == IDYES)
            s_cancel = 1;
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc, t, bar, fill;
        HDC dc = BeginPaint(h, &ps);
        HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

        GetClientRect(h, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        t = rc;
        t.left += 16; t.right -= 16; t.top += 14; t.bottom = t.top + 18;
        DrawTextA(dc, "Unpacking the game ISO (first start only)...", -1, &t,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        OffsetRect(&t, 0, 20);
        DrawTextA(dc, s_line1, -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        OffsetRect(&t, 0, 20);
        DrawTextA(dc, s_line2, -1, &t,
                  DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        bar = rc;
        bar.left += 16; bar.right -= 16; bar.top = t.bottom + 10;
        bar.bottom = bar.top + 18;
        FrameRect(dc, &bar, GetSysColorBrush(COLOR_WINDOWFRAME));
        fill = bar;
        InflateRect(&fill, -2, -2);
        fill.right = fill.left + (fill.right - fill.left) * s_permille / 1000;
        FillRect(dc, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void progress_open(void)
{
    WNDCLASSA wc;
    int w = 480, ht = 150;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "SCDAUnpackProgress";
    RegisterClassA(&wc);
    s_wnd = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, s_caption,
                            WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - ht) / 2, w, ht,
                            NULL, NULL, wc.hInstance, NULL);
    if (s_wnd) {
        ShowWindow(s_wnd, SW_SHOWNORMAL);
        UpdateWindow(s_wnd);
    }
}

static void progress_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void progress_close(void)
{
    if (s_wnd) {
        DestroyWindow(s_wnd);
        s_wnd = NULL;
    }
    progress_pump();
}
#else
/* no native dialog on Linux (ARM port, docs Update 677): progress on the
 * console, about once a second */
static void progress_open(void) {}
static void progress_close(void) {}
static void progress_pump(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    fprintf(stderr, "[SCDA] unpacking the game ISO: %s\n", s_line1);
}
#define InvalidateRect(w, r, e) ((void)0)
#endif

/* ---- XISO reader ------------------------------------------------------ */

typedef struct {
    HANDLE iso;
    uint64_t base;            /* game partition offset in the image */
    uint32_t root_sector, root_size;
    uint64_t total, done;     /* bytes of all files / copied so far */
    unsigned files;
    unsigned char *buf;
    DWORD last_tick;
    int error;
    char msg[512];
} Xiso;

static int iso_read(Xiso *x, uint64_t off, void *dst, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(x->iso, li, NULL, FILE_BEGIN) &&
           ReadFile(x->iso, dst, len, &got, NULL) && got == len;
}

static int xiso_open(Xiso *x, const char *path)
{
    /* trimmed XISO, Redump full image, the two other full-image layouts
     * extract-xiso knows */
    static const uint64_t bases[] = { 0, 0x18300000ull, 0x0FD90000ull,
                                      0x02080000ull };
    unsigned char vd[32];

    memset(x, 0, sizeof(*x));
    x->iso = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (x->iso == INVALID_HANDLE_VALUE)
        return 0;
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        if (iso_read(x, bases[i] + 32ull * XDVD_SECTOR, vd, sizeof(vd)) &&
            memcmp(vd, XDVD_MAGIC, 20) == 0) {
            x->base = bases[i];
            x->root_sector = *(uint32_t *)(vd + 20);
            x->root_size = *(uint32_t *)(vd + 24);
            return 1;
        }
    }
    CloseHandle(x->iso);
    x->iso = INVALID_HANDLE_VALUE;
    return 0;
}

/* XDVDFS names are plain ASCII; refuse anything that could leave the
 * output folder. */
static int safe_name(const char *n)
{
    if (!n[0] || !strcmp(n, ".") || !strcmp(n, ".."))
        return 0;
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E || strchr("\\/:*?\"<>|", c))
            return 0;
    }
    return 1;
}

static void progress_update(Xiso *x, const char *path, int force)
{
    DWORD now = GetTickCount();
    if (!force && now - x->last_tick < 120)
        return;
    x->last_tick = now;
    s_permille = x->total ? (int)(x->done * 1000 / x->total) : 0;
    _snprintf_s(s_line1, sizeof(s_line1), _TRUNCATE,
                "%d %%  (%.1f of %.1f GB)", s_permille / 10,
                x->done / 1073741824.0, x->total / 1073741824.0);
    if (path)
        _snprintf_s(s_line2, sizeof(s_line2), _TRUNCATE, "%s", path);
    if (s_wnd)
        InvalidateRect(s_wnd, NULL, FALSE);
    progress_pump();
}

static int copy_file(Xiso *x, uint32_t sector, uint32_t size, const char *path)
{
    HANDLE out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    uint64_t src = x->base + (uint64_t)sector * XDVD_SECTOR;
    uint32_t left = size;

    if (out == INVALID_HANDLE_VALUE) {
        _snprintf_s(x->msg, sizeof(x->msg), _TRUNCATE,
                    "Could not create\n%s\n(error %lu)", path, GetLastError());
        return 0;
    }
    while (left) {
        DWORD n = left < COPY_CHUNK ? left : COPY_CHUNK, wrote = 0;
        if (!iso_read(x, src, x->buf, n)) {
            _snprintf_s(x->msg, sizeof(x->msg), _TRUNCATE,
                        "Read error in the ISO at %s", path);
            CloseHandle(out);
            return 0;
        }
        if (!WriteFile(out, x->buf, n, &wrote, NULL) || wrote != n) {
            _snprintf_s(x->msg, sizeof(x->msg), _TRUNCATE,
                        "Could not write\n%s\n(error %lu - disk full?)", path,
                        GetLastError());
            CloseHandle(out);
            return 0;
        }
        src += n;
        left -= n;
        x->done += n;
        progress_update(x, path, 0);
        if (s_cancel) {
            CloseHandle(out);
            return 0;
        }
    }
    CloseHandle(out);
    return 1;
}

/* out_dir NULL: count only (x->total, x->files). */
static int xiso_walk(Xiso *x, uint32_t sector, uint32_t size,
                     const char *out_dir, int depth)
{
    unsigned char *d;
    uint32_t *stack;
    unsigned char *seen;
    uint32_t alloc, sp = 0, cap;
    int ok = 1;

    if (size == 0)
        return 1;                       /* empty directory */
    if (depth > 32 || size > (64u << 20)) {
        strcpy_s(x->msg, sizeof(x->msg), "The ISO's directory tree is damaged.");
        return 0;
    }
    alloc = (size + XDVD_SECTOR - 1) / XDVD_SECTOR * XDVD_SECTOR;
    d = (unsigned char *)malloc(alloc);
    cap = size / 4 + 1;
    stack = (uint32_t *)malloc(cap * sizeof(uint32_t));
    seen = (unsigned char *)calloc(cap, 1);
    if (!d || !stack || !seen ||
        !iso_read(x, x->base + (uint64_t)sector * XDVD_SECTOR, d, alloc)) {
        strcpy_s(x->msg, sizeof(x->msg), "Could not read a directory of the ISO.");
        free(d); free(stack); free(seen);
        return 0;
    }

    stack[sp++] = 0;
    while (sp && ok && !s_cancel) {
        uint32_t o = stack[--sp];
        uint16_t left, right;
        uint32_t fsec, fsize;
        unsigned char attr, nlen;
        char name[256], path[MAX_PATH];

        if (o + 14 > size || seen[o / 4])
            continue;
        seen[o / 4] = 1;
        left = *(uint16_t *)(d + o);
        right = *(uint16_t *)(d + o + 2);
        if (left == 0xFFFF)
            continue;                   /* sector padding */
        fsec = *(uint32_t *)(d + o + 4);
        fsize = *(uint32_t *)(d + o + 8);
        attr = d[o + 12];
        nlen = d[o + 13];
        if (o + 14 + nlen > size)
            continue;
        if (left && sp < cap)
            stack[sp++] = (uint32_t)left * 4;
        if (right && sp < cap)
            stack[sp++] = (uint32_t)right * 4;
        memcpy(name, d + o + 14, nlen);
        name[nlen] = '\0';
        if (!safe_name(name))
            continue;
        if (out_dir)
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", out_dir, name);

        if (attr & 0x10) {
            if (out_dir && !CreateDirectoryA(path, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                _snprintf_s(x->msg, sizeof(x->msg), _TRUNCATE,
                            "Could not create the folder\n%s", path);
                ok = 0;
                break;
            }
            ok = xiso_walk(x, fsec, fsize, out_dir ? path : NULL, depth + 1);
        } else if (!out_dir) {
            x->total += fsize;
            x->files++;
        } else {
            ok = copy_file(x, fsec, fsize, path);
        }
    }
    free(d);
    free(stack);
    free(seen);
    return ok && !s_cancel;
}

/* Unpack iso into out_dir. Returns 1 on success; shows its own errors. */
static int unpack_iso(const char *iso_path, const char *out_dir)
{
    Xiso x;
    ULARGE_INTEGER free_bytes;
    char text[1024], marker[MAX_PATH];
    int ok;

    if (!xiso_open(&x, iso_path))
        return 0;

    if (!xiso_walk(&x, x.root_sector, x.root_size, NULL, 0)) {
        CloseHandle(x.iso);
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%s\n\n%s", x.msg, iso_path);
        fail(text);
    }
    {
        char *slash;
        strcpy_s(marker, sizeof(marker), out_dir);   /* its parent folder */
        slash = strrchr(marker, '\\');
        if (slash)
            slash[1] = '\0';
    }
    if (GetDiskFreeSpaceExA(marker, &free_bytes, NULL, NULL)) {
        if (free_bytes.QuadPart < x.total + (64ull << 20)) {
            CloseHandle(x.iso);
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "Not enough free disk space to unpack the game ISO:\n"
                        "%.1f GB needed, %.1f GB free.\n\n"
                        "Free some space, or unpack the ISO yourself and put "
                        "the game folder into \"gamefiles\".",
                        x.total / 1073741824.0,
                        free_bytes.QuadPart / 1073741824.0);
            fail(text);
        }
    }

    CreateDirectoryA(out_dir, NULL);
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\" EXTRACT_MARKER, out_dir);
    DeleteFileA(marker);

    x.buf = (unsigned char *)malloc(COPY_CHUNK);
    if (!x.buf)
        fail("Out of memory while unpacking the ISO.");
    s_cancel = 0;
    progress_open();
    progress_update(&x, out_dir, 1);
    ok = xiso_walk(&x, x.root_sector, x.root_size, out_dir, 0);
    progress_close();
    free(x.buf);
    CloseHandle(x.iso);

    if (s_cancel)
        ExitProcess(0);
    if (!ok) {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "Unpacking the game ISO failed:\n\n%s", x.msg);
        fail(text);
    }
    {
        FILE *f = NULL;
        if (fopen_s(&f, marker, "wb") == 0 && f) {
            fprintf(f, "Unpacked from %s: %u files, %llu bytes.\r\n"
                    "Delete this folder to unpack the ISO again.\r\n",
                    iso_path, x.files, (unsigned long long)x.total);
            fclose(f);
        }
        SetFileAttributesA(marker, FILE_ATTRIBUTE_HIDDEN);
    }
    return 1;
}

/* ---- folder search ---------------------------------------------------- */

/* gamefiles\<base>.iso / .xiso for a folder name, if present */
static int iso_for_folder(const char *gamefiles, const char *folder)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s.iso", gamefiles, folder);
    if (is_file(p))
        return 1;
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s.xiso", gamefiles, folder);
    return is_file(p);
}

static int find_game_folder(const char *gamefiles, char *out, size_t out_size)
{
    char p[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int found = 0;

    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\default.xbe", gamefiles);
    if (is_file(p)) {
        _snprintf_s(out, out_size, _TRUNCATE, "%s", gamefiles);
        return 1;
    }
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\*", gamefiles);
    h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char xbe[MAX_PATH], marker[MAX_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        _snprintf_s(xbe, sizeof(xbe), _TRUNCATE, "%s\\%s\\default.xbe",
                    gamefiles, fd.cFileName);
        if (!is_file(xbe))
            continue;
        /* a folder unpacked from an ISO counts only once finished */
        _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\%s\\" EXTRACT_MARKER,
                    gamefiles, fd.cFileName);
        if (iso_for_folder(gamefiles, fd.cFileName) && !is_file(marker))
            continue;
        _snprintf_s(out, out_size, _TRUNCATE, "%s\\%s", gamefiles, fd.cFileName);
        found = 1;
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static int find_and_unpack_iso(const char *gamefiles, char *out, size_t out_size)
{
    static const char *const patterns[] = { "*.iso", "*.xiso" };
    int seen_iso = 0;

    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        char p[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;

        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", gamefiles, patterns[i]);
        h = FindFirstFileA(p, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            char iso[MAX_PATH], dir[MAX_PATH], *dot;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            seen_iso = 1;
            _snprintf_s(iso, sizeof(iso), _TRUNCATE, "%s\\%s", gamefiles,
                        fd.cFileName);
            _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", gamefiles,
                        fd.cFileName);
            dot = strrchr(dir, '.');
            if (dot)
                *dot = '\0';
            if (unpack_iso(iso, dir)) {
                FindClose(h);
                _snprintf_s(out, out_size, _TRUNCATE, "%s", dir);
                return 1;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return seen_iso ? -1 : 0;
}

/* The DLLs shipped next to the EXE. glslang.dll is delay-loaded: without it
 * the renderer thread died with exception C06D007E and only a minidump
 * (a player copied just the EXE and the settings file, 2026-10-13). */
static void check_runtime_files(const char *exe_dir)
{
    static const char *const dlls[] = {
        "glslang.dll", "libSPIRV-Tools-opt.dll", "libSPIRV-Tools.dll",
        "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll",
        "dsp56300.dll",
    };
    char p[MAX_PATH], missing[512] = "", text[1024];

#if !defined(_WIN32)
    (void)dlls; (void)p; (void)text; (void)exe_dir;
    return;   /* Linux links its libraries (ARM port, docs Update 677) */
#endif
    for (size_t i = 0; i < sizeof(dlls) / sizeof(dlls[0]); i++) {
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", exe_dir, dlls[i]);
        if (!is_file(p)) {
            strcat_s(missing, sizeof(missing), "\n  ");
            strcat_s(missing, sizeof(missing), dlls[i]);
        }
    }
    if (missing[0]) {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "Files missing next to scda_recomp.exe:\n%s\n\n"
                    "Extract the complete zip file (all .dll files belong "
                    "next to the .exe).", missing);
        fail(text);
    }
}

int scda_layout_resolve(const char *exe_dir, char *game_dir, size_t game_size,
                        char *user_dir, size_t user_size)
{
    char p[MAX_PATH], gamefiles[MAX_PATH], text[1024];
    int r;

    /* development layout: the game next to the EXE */
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\default.xbe", exe_dir);
    if (is_file(p)) {
        check_runtime_files(exe_dir);
        return 0;
    }

    /* The console a double-click opens shows nothing (the log goes to a
     * file); fb_present.c hides it with the game window, but the messages
     * and the unpack window here come first. Same rule: only when no
     * command prompt shares it. */
    {
        DWORD procs[2];
        HWND con = GetConsoleWindow();
        const char *keep = getenv("RECOMP_SHOW_CONSOLE");
        if (con && !(keep && *keep && *keep != '0') &&
            GetConsoleProcessList(procs, 2) == 1)
            ShowWindow(con, SW_HIDE);
    }
    check_runtime_files(exe_dir);

    _snprintf_s(gamefiles, sizeof(gamefiles), _TRUNCATE, "%s\\gamefiles", exe_dir);
    _snprintf_s(user_dir, user_size, _TRUNCATE, "%s\\user", exe_dir);
    CreateDirectoryA(gamefiles, NULL);
    CreateDirectoryA(user_dir, NULL);
    if (!is_dir(gamefiles) || !is_dir(user_dir)) {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "Could not create the folders \"gamefiles\" and \"user\" "
                    "next to the program:\n\n%s\n\nMove the game to a folder "
                    "you can write to (not Program Files).", exe_dir);
        fail(text);
    }

    if (!find_game_folder(gamefiles, game_dir, game_size)) {
        r = find_and_unpack_iso(gamefiles, game_dir, game_size);
        if (r <= 0) {
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "Game files missing!\n\n"
                        "Place the ISO or the game folder (with default.xbe) "
                        "in the \"gamefiles\" folder:\n\n%s%s", gamefiles,
                        r < 0 ? "\n\nThe ISO found there is not an Xbox game "
                                "image (XISO or full disc image)." : "");
            fail(text);
        }
    }
    check_game(game_dir);
    return 1;
}
