/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_PinDeviceThread(void *h);

static volatile LONG s_fb_running;
static volatile LONG s_burst_generation;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */
/* Window client size and the size of what s_rgb currently holds. With
 * RECOMP_SURFACE_SCALE=N (docs Update 473) the GPU renders at N times the
 * guest resolution and the window shows it at that size; the guest-RAM
 * scan-out below stays 640x480 and is stretched. */
static uint32_t      s_out_w = 640, s_out_h = 480, s_img_w = 640, s_img_h = 480;

/* A device that renders the scan-out itself (the emulated NV2A, src/gpu)
 * hands finished XRGB pixels here instead of the window reading guest RAM:
 * the GPU's image lives in its own surfaces and reaches RAM only when
 * something downloads it, so reading RAM showed stale or half-written
 * frames (docs Update 355, flicker). Returns nonzero when it filled dst;
 * zero falls back to the guest-RAM scan-out below. */
typedef int (*fb_host_source_fn)(uint32_t *dst, uint32_t width, uint32_t height);
static volatile fb_host_source_fn s_host_source;

void xbox_FramebufferWindowSetSource(fb_host_source_fn fn)
{
    s_host_source = fn;
}

/* A device that can draw the scan-out straight into the window with its own
 * GPU API (the emulated NV2A: a GL blit plus SwapBuffers) registers this; it
 * returns nonzero when it presented. The readback + CPU scale + GDI path above
 * showed ~32 refreshes a second while the guest flipped 60 (docs Updates
 * 478/479). RECOMP_FB_GDI=1 keeps the old path; frame dumps (F11,
 * RECOMP_FB_DUMP) use it for the frames they need pixels of. */
typedef int (*fb_host_present_fn)(void *hwnd, uint32_t width, uint32_t height);
static volatile fb_host_present_fn s_host_present;
static int s_burst_left;
/* xbox_FramebufferBurstStart (F8 draw trace in src/gpu/nv2a_recomp.c) asks
 * for an F11-style burst from another thread. */
static volatile LONG s_burst_request;
extern int nv2a_recomp_frame_time(void);

/* Opt-in two-phase performance marker for comparing a scene before and
 * after an in-game action. F7 is unused by the normal input and F8/F9/F10/
 * F11/F12 diagnostics. No framebuffer capture or gameplay state changes. */
static int s_light_probe_enabled = -1;
static int s_light_probe_phase;

void xbox_FramebufferBurstStart(void)
{
    InterlockedExchange(&s_burst_request, 1);
}

void xbox_FramebufferWindowSetPresenter(fb_host_present_fn fn)
{
    s_host_present = fn;
}

/* The picture's aspect ratio as the window was opened with it (4:3, or
 * 16:9 with RECOMP_WIDESCREEN): a presenter fits the image into the client
 * area at this ratio and fills the rest black, so resizing the window never
 * distorts it (docs Update 483). */
void xbox_FramebufferWindowAspect(uint32_t *w, uint32_t *h)
{
    *w = s_out_w;
    *h = s_out_h;
}

unsigned xbox_FramebufferBurstGeneration(void)
{
    return (unsigned)InterlockedCompareExchange(&s_burst_generation, 0, 0);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* ---- Window handover across a title relaunch (docs Update 502) ----
 * On a console XLaunchNewImage reboots into the new image while the screen
 * keeps showing the last frame until the new image draws. Here a relaunch is
 * a new process (kernel_bridge.c bridge_relaunch_same_title), and its window
 * replaced the old one with a gap. Now the old process passes its window
 * (RECOMP_HANDOVER_HWND) and an event (RECOMP_HANDOVER_EVENT) and keeps
 * showing its last frame; the new process opens its window hidden, puts it
 * at the old window's place just BEHIND it once it has a frame, presents a
 * few frames there, brings it to the front and sets the event -- then the
 * old process exits. */
static HWND s_hwnd;

void *xbox_FramebufferWindowHandle(void)
{
    return s_hwnd;
}

typedef struct {
    HWND   prev;          /* the old instance's window */
    HANDLE done;          /* set when this window has taken over */
    WINDOWPLACEMENT wp;   /* the old window's placement */
    int    state;         /* 0 hidden, 1 behind the old window, 2 done */
    int    frames;        /* presents since state 1 */
    DWORD  t_start;
} FbHandover;
static FbHandover s_ho;

static int handover_begin(HWND hwnd)
{
    char v[64];
    DWORD n = GetEnvironmentVariableA("RECOMP_HANDOVER_HWND", v, sizeof v);
    HWND prev;

    if (!n || n >= sizeof v)
        return 0;
    prev = (HWND)(uintptr_t)_strtoui64(v, NULL, 16);
    n = GetEnvironmentVariableA("RECOMP_HANDOVER_EVENT", v, sizeof v);
    /* Consumed: a later relaunch of this instance passes its own. */
    SetEnvironmentVariableA("RECOMP_HANDOVER_HWND", NULL);
    SetEnvironmentVariableA("RECOMP_HANDOVER_EVENT", NULL);
    if (!prev || !IsWindow(prev) || !n || n >= sizeof v)
        return 0;
    s_ho.done = OpenEventA(EVENT_MODIFY_STATE, FALSE, v);
    s_ho.wp.length = sizeof s_ho.wp;
    if (!s_ho.done || !GetWindowPlacement(prev, &s_ho.wp)) {
        if (s_ho.done)
            CloseHandle(s_ho.done);
        return 0;
    }
    s_ho.prev = prev;
    s_ho.t_start = GetTickCount();
    {
        /* Restore position for later; stays hidden for now. */
        WINDOWPLACEMENT wp = s_ho.wp;
        wp.showCmd = SW_HIDE;
        wp.flags = 0;
        SetWindowPlacement(hwnd, &wp);
    }
    fprintf(stderr, "  [FBWIN] relaunch: taking over window %p\n", (void *)prev);
    return 1;
}

/* Whether the new instance's current frame shows anything: the console keeps
 * the old picture until the new image draws its loading screen, and the
 * first frames after a start are black. Reads the frame back (only during a
 * handover) and samples every 7th pixel. */
static int handover_frame_has_content(void)
{
    size_t n, i, lit = 0;

    if (!s_host_source || !s_rgb)
        return 1;
    if (!s_host_source(s_rgb, s_out_w, s_out_h))
        return 0;
    n = (size_t)s_out_w * s_out_h;
    for (i = 0; i < n; i += 7) {
        uint32_t p = s_rgb[i];
        if (((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF) > 3 * 24)
            lit++;
    }
    return lit * 7 * 200 > n;   /* more than 0.5 % of the picture */
}

/* Called after every window refresh while a handover is pending; new_frame
 * is nonzero when the refresh showed a new guest frame. */
static void handover_step(HWND hwnd, int new_frame)
{
    /* A new instance that never draws must not stay invisible. */
    int late = GetTickCount() - s_ho.t_start > 30000;

    if (s_ho.state == 0 && ((new_frame && handover_frame_has_content()) || late)) {
        RECT r;
        if (IsWindow(s_ho.prev) && GetWindowRect(s_ho.prev, &r))
            SetWindowPos(hwnd, s_ho.prev, r.left, r.top, r.right - r.left,
                         r.bottom - r.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        else
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        s_ho.state = 1;
        s_ho.frames = 0;
        return;
    }
    if (s_ho.state == 1 && ((new_frame && ++s_ho.frames >= 3) || late)) {
        if (s_ho.wp.showCmd == SW_SHOWMAXIMIZED)
            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        SetEvent(s_ho.done);
        CloseHandle(s_ho.done);
        s_ho.done = NULL;
        s_ho.state = 2;
        fprintf(stderr, "  [FBWIN] relaunch: window taken over\n");
    }
}

/* ── Window options from scda_options.txt (docs Update 614) ──────────────
 * RECOMP_FULLSCREEN=1  borderless window covering the start monitor
 * RECOMP_MONITOR=<n>   start monitor: 1 = primary, 2.. = the others
 * RECOMP_REMEMBER_WINDOW=1  window size/position kept in scda_window.txt
 *                      next to the EXE (windowed mode only) */
static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && *v != '0';
}

typedef struct { RECT r[16]; int n; } MonitorList;

static BOOL CALLBACK monitor_collect(HMONITOR m, HDC dc, LPRECT rc, LPARAM p)
{
    MonitorList *ml = (MonitorList *)p;
    MONITORINFO mi;
    (void)dc;
    (void)rc;
    mi.cbSize = sizeof mi;
    if (ml->n < 16 && GetMonitorInfoA(m, &mi)) {
        if (mi.dwFlags & MONITORINFOF_PRIMARY) {
            /* the primary is number 1 */
            memmove(&ml->r[1], &ml->r[0], sizeof(RECT) * (size_t)ml->n);
            ml->r[0] = mi.rcMonitor;
        } else {
            ml->r[ml->n] = mi.rcMonitor;
        }
        ml->n++;
    }
    return TRUE;
}

static RECT start_monitor_rect(void)
{
    MonitorList ml;
    const char *v = getenv("RECOMP_MONITOR");
    int idx = v ? atoi(v) : 1;
    ml.n = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_collect, (LPARAM)&ml);
    if (!ml.n) {
        RECT r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        return r;
    }
    if (idx < 1 || idx > ml.n)
        idx = 1;
    return ml.r[idx - 1];
}

static void window_state_path(char *buf, DWORD size)
{
    /* RECOMP_DATA_DIR: the release layout's "user" folder (see glsl.c) */
    const char *data = getenv("RECOMP_DATA_DIR");
    if (data && data[0]) {
        _snprintf_s(buf, size, _TRUNCATE, "%s\\scda_window.txt", data);
        return;
    }
    DWORD n = GetModuleFileNameA(NULL, buf, size);
    while (n && buf[n - 1] != '\\')
        n--;
    buf[n] = '\0';
    strncat_s(buf, size, "scda_window.txt", _TRUNCATE);
}

static void window_save(HWND hwnd)
{
    char path[MAX_PATH];
    WINDOWPLACEMENT wp;
    FILE *f;
    if (!env_on("RECOMP_REMEMBER_WINDOW") || env_on("RECOMP_FULLSCREEN"))
        return;
    wp.length = sizeof wp;
    if (!GetWindowPlacement(hwnd, &wp))
        return;
    window_state_path(path, MAX_PATH);
    if (fopen_s(&f, path, "w") == 0 && f) {
        fprintf(f, "%ld %ld %ld %ld %d\n", wp.rcNormalPosition.left,
                wp.rcNormalPosition.top, wp.rcNormalPosition.right,
                wp.rcNormalPosition.bottom, wp.showCmd == SW_SHOWMAXIMIZED);
        fclose(f);
    }
}

/* Places and shows a new (not handed-over) window. */
static void window_place_start(HWND hwnd)
{
    RECT mon = start_monitor_rect();
    if (env_on("RECOMP_FULLSCREEN")) {
        SetWindowPos(hwnd, HWND_TOP, mon.left, mon.top, mon.right - mon.left,
                     mon.bottom - mon.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        return;
    }
    if (env_on("RECOMP_REMEMBER_WINDOW")) {
        char path[MAX_PATH];
        FILE *f;
        long l, t, r, b;
        int max;
        window_state_path(path, MAX_PATH);
        if (fopen_s(&f, path, "r") == 0 && f) {
            int ok = fscanf_s(f, "%ld %ld %ld %ld %d", &l, &t, &r, &b, &max) == 5;
            RECT rc = { l, t, r, b };
            fclose(f);
            /* Only where a monitor still is. */
            if (ok && r - l >= 160 && b - t >= 120 &&
                MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {
                WINDOWPLACEMENT wp;
                memset(&wp, 0, sizeof wp);
                wp.length = sizeof wp;
                wp.rcNormalPosition = rc;
                wp.showCmd = max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
                SetWindowPlacement(hwnd, &wp);
                return;
            }
        }
    }
    {
        /* Centred on the start monitor. */
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        int x = mon.left + ((mon.right - mon.left) - w) / 2;
        int y = mon.top + ((mon.bottom - mon.top) - h) / 2;
        if (x < mon.left) x = mon.left;
        if (y < mon.top) y = mon.top;
        SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE)
        window_save(h);
    /* Closing the game window ends the game (docs Update 613). Stopping only
     * the present left the title running with no window, to be killed in
     * the Task Manager. Flush the logs, then terminate at once: ExitProcess
     * would run DLL detach (graphics and audio drivers) under the loader lock
     * with every guest and device thread still live. The OS releases the
     * audio device, including an exclusive Dolby Digital passthrough. */
    if (m == WM_CLOSE && !getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
        fprintf(stderr, "  [FBWIN] window closed by the user: ending the program\n");
        fflush(NULL);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (m == WM_CLOSE || m == WM_DESTROY) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_img_w * 3u) + 3u) & ~3u;
    uint32_t img = row * s_img_h, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || (!s_fb_va && !s_host_source))
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_img_w, 4);
    memcpy(hdr + 22, &s_img_h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_img_h; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_img_h - 1 - y) * s_img_w;
        for (x = 0; x < s_img_w; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_img_w, s_img_h, s_fb_va);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;
    /* The display is not guest code; keep it off the guest's core. */
    xbox_PinDeviceThread(GetCurrentThread());

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        const char *sc = getenv("RECOMP_SURFACE_SCALE");
        int scale = sc ? atoi(sc) : 1;
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;
        s_out_w = s_fb_width * (uint32_t)scale;
        s_out_h = s_fb_height * (uint32_t)scale;
        /* RECOMP_WIDESCREEN=1 (docs Update 481): the title draws 16:9
         * anamorphic into its 4:3 target; open the window 16:9 so the
         * present stretches it back. */
        {
            const char *ws = getenv("RECOMP_WIDESCREEN");
            if (ws && *ws && *ws != '0')
                s_out_w = s_out_h * 16u / 9u;
        }
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_out_w; r.bottom = (LONG)s_out_h;
    AdjustWindowRect(&r, env_on("RECOMP_FULLSCREEN") ? WS_POPUP
                                                     : WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           env_on("RECOMP_FULLSCREEN") ? WS_POPUP
                                                       : WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    if (!handover_begin(hwnd))
        window_place_start(hwnd);
    s_hwnd = hwnd;
    /* Only the game window (docs Update 613): the console the runtime got
     * when started by double-click shows nothing -- the log goes to
     * scda_runtime.log -- so hide it, but only when no other process (a
     * command prompt the user typed into) shares it.
     * RECOMP_SHOW_CONSOLE=1 keeps it. */
    {
        DWORD procs[2];
        HWND con = GetConsoleWindow();
        const char *keep = getenv("RECOMP_SHOW_CONSOLE");
        if (con && !(keep && *keep && *keep != '0') &&
            GetConsoleProcessList(procs, 2) == 1)
            ShowWindow(con, SW_HIDE);
    }
    hdc = GetDC(hwnd);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_out_w * s_out_h, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_out_w, s_out_h);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        {
            static int f7_was_down;
            static DWORD phase_deadline;
            DWORD now = GetTickCount();
            if (s_light_probe_enabled < 0) {
                const char *setting = getenv("RECOMP_SCDA_LIGHT_PROBE");
                s_light_probe_enabled = setting && setting[0] == '1';
            }
            if (s_light_probe_enabled) {
                DWORD focus_pid = 0;
                HWND focus = GetForegroundWindow();
                if (focus)
                    GetWindowThreadProcessId(focus, &focus_pid);
                int f7_down = focus_pid == GetCurrentProcessId() &&
                              (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
                if (s_light_probe_phase == 1 &&
                    (int32_t)(now - phase_deadline) >= 0) {
                    fprintf(stderr,
                            "[LIGHT-PROBE] phase=BEFORE_END tick=%lu frame=%d\n",
                            (unsigned long)now, nv2a_recomp_frame_time());
                    s_light_probe_phase = 2;
                } else if (s_light_probe_phase == 3 &&
                           (int32_t)(now - phase_deadline) >= 0) {
                    fprintf(stderr,
                            "[LIGHT-PROBE] phase=AFTER_END tick=%lu frame=%d\n",
                            (unsigned long)now, nv2a_recomp_frame_time());
                    s_light_probe_phase = 4;
                }
                if (f7_down && !f7_was_down) {
                    if (s_light_probe_phase == 0) {
                        s_light_probe_phase = 1;
                        phase_deadline = now + 18000;
                        fprintf(stderr,
                                "[LIGHT-PROBE] phase=BEFORE_BEGIN tick=%lu frame=%d\n",
                                (unsigned long)now, nv2a_recomp_frame_time());
                    } else if (s_light_probe_phase == 2) {
                        s_light_probe_phase = 3;
                        phase_deadline = now + 18000;
                        fprintf(stderr,
                                "[LIGHT-PROBE] phase=AFTER_BEGIN tick=%lu frame=%d\n",
                                (unsigned long)now, nv2a_recomp_frame_time());
                    }
                }
                f7_was_down = f7_down;
            }
        }
        int presented = 0, new_frame = 0;
        {
            static int gdi = -1;
            RECT cr;
            if (gdi < 0)
                gdi = getenv("RECOMP_FB_GDI") != NULL;
            if (!gdi && s_host_present && !s_burst_left && !getenv("RECOMP_FB_DUMP") &&
                GetClientRect(hwnd, &cr) && cr.right > 0 && cr.bottom > 0)
                presented = s_host_present(hwnd, (uint32_t)cr.right, (uint32_t)cr.bottom);
        }
        if (presented) {
            static DWORD t0, t_title;
            static unsigned shown, shown_title;
            DWORD now = GetTickCount();
            if (presented == 1) {
                /* [FRAME-HITCH] (docs Update 621): a game frame that took
                 * over 40 ms, with the tick, to line up with audio events. */
                static LARGE_INTEGER f, last;
                LARGE_INTEGER t;
                QueryPerformanceCounter(&t);
                if (!f.QuadPart)
                    QueryPerformanceFrequency(&f);
                if (last.QuadPart && shown > 300) {
                    double ms = (t.QuadPart - last.QuadPart) * 1000.0 / f.QuadPart;
                    static unsigned logged;
                    if (ms > 40.0 && (logged++ < 500 || (logged & (logged - 1)) == 0))
                        fprintf(stderr, "[FRAME-HITCH] %.1f ms frame at tick %lu\n",
                                ms, (unsigned long)GetTickCount());
                }
                last = t;
                new_frame = 1;
                shown++;
                shown_title++;
            }
            /* Game frames per second in the title bar, once a second
             * (docs Update 482): presents happen only for new guest
             * frames, so this is the game's own rate. */
            if (!t_title)
                t_title = now;
            if (now - t_title >= 1000) {
                char title[128];
                unsigned fps =
                    (unsigned)((shown_title * 1000u + (now - t_title) / 2) /
                               (now - t_title));
                if (s_light_probe_enabled) {
                    static const char *phase[] = {
                        "F7: BEFORE", "BEFORE (wait)", "SHOOT, then F7",
                        "AFTER (wait)", "DONE"
                    };
                    snprintf(title, sizeof(title),
                             "Xbox Recomp - %u FPS | Light: %s", fps,
                             phase[s_light_probe_phase]);
                } else {
                    snprintf(title, sizeof(title), "Xbox Recomp - %u FPS", fps);
                }
                SetWindowTextA(hwnd, title);
                shown_title = 0;
                t_title = now;
            }
            if (!t0)
                t0 = now;
            if (now - t0 >= 5000) {
                fprintf(stderr, "[FBWIN-RATE] %u window refreshes in %lu ms (GL present)\n",
                        shown, (unsigned long)(now - t0));
                shown = 0;
                t0 = now;
            }
        } else if (s_host_source && s_rgb &&
            s_host_source(s_rgb, s_out_w, s_out_h)) {
            s_img_w = s_out_w;
            s_img_h = s_out_h;
            bi.bmiHeader.biWidth  = (LONG)s_img_w;
            bi.bmiHeader.biHeight = -(LONG)s_img_h;
            StretchDIBits(hdc, 0, 0, (int)s_out_w, (int)s_out_h,
                          0, 0, (int)s_img_w, (int)s_img_h,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
            new_frame = 1;
            {
                /* Window refresh rate next to [GPU-STAT]'s guest flip rate:
                 * the GPU can flip faster than this readback + GDI path
                 * shows (docs Update 478). One line per 5 s. */
                static DWORD t0;
                static unsigned shown;
                DWORD now = GetTickCount();
                shown++;
                if (!t0)
                    t0 = now;
                if (now - t0 >= 5000) {
                    fprintf(stderr, "[FBWIN-RATE] %u window refreshes in %lu ms (%ux%u)\n",
                            shown, (unsigned long)(now - t0), s_out_w, s_out_h);
                    shown = 0;
                    t0 = now;
                }
            }
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            /* Diagnostics (docs Update 345): how long one refresh holds the
             * core. One line per 256 refreshes with RECOMP_FB_TIME=1. */
            static LARGE_INTEGER qf;
            static double t_conv, t_blit, t_max;
            static unsigned n_iter;
            LARGE_INTEGER a, b, c;
            if (!qf.QuadPart)
                QueryPerformanceFrequency(&qf);
            QueryPerformanceCounter(&a);
            fb_convert(src, s_fb_pitch / s_fb_width);
            s_img_w = s_fb_width;
            s_img_h = s_fb_height;
            bi.bmiHeader.biWidth  = (LONG)s_img_w;
            bi.bmiHeader.biHeight = -(LONG)s_img_h;
            QueryPerformanceCounter(&b);
            StretchDIBits(hdc, 0, 0, (int)s_out_w, (int)s_out_h,
                          0, 0, (int)s_img_w, (int)s_img_h,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
            new_frame = 1;
            QueryPerformanceCounter(&c);
            t_conv += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart;
            t_blit += (double)(c.QuadPart - b.QuadPart) * 1000.0 / (double)qf.QuadPart;
            if ((double)(c.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart > t_max)
                t_max = (double)(c.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart;
            if (++n_iter % 256 == 0 && getenv("RECOMP_FB_TIME")) {
                fprintf(stderr, "  [FBWIN-TIME] %u refreshes: avg convert %.2f ms, "
                        "avg blit %.2f ms, max %.2f ms (va=0x%08X pitch=%u)\n",
                        n_iter, t_conv / 256, t_blit / 256, t_max, s_fb_va,
                        s_fb_pitch);
                t_conv = t_blit = t_max = 0;
            }
        }
        if (s_ho.prev && s_ho.state < 2)
            handover_step(hwnd, new_frame);
        {
            /* F11 (window focused): dump the next 60 refreshes, ~1 s, as
             * fbburst_<n>_<i>.bmp next to the exe -- to see what changes from
             * frame to frame in a flickering scene (docs Update 357). */
            static int burst_no, f11_was_down;
            int f11_down = GetForegroundWindow() == hwnd &&
                           (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if ((f11_down && !f11_was_down && !s_burst_left) ||
                (InterlockedExchange(&s_burst_request, 0) && !s_burst_left)) {
                s_burst_left = 60;
                burst_no++;
                InterlockedIncrement(&s_burst_generation);
                fprintf(stderr, "  [FBWIN] F11: dumping 60 frames (burst %d)\n",
                        burst_no);
            }
            f11_was_down = f11_down;
            if (s_burst_left) {
                char path[MAX_PATH];
                /* RECOMP_FB_BURST_DIR: write the burst there (with this
                 * run's process id in the name) instead of next to the exe,
                 * so earlier bursts are never overwritten (docs Update 496). */
                const char *bdir = getenv("RECOMP_FB_BURST_DIR");
                if (bdir && *bdir) {
                    CreateDirectoryA(bdir, NULL);
                    snprintf(path, sizeof(path), "%s\\fbburst_%lu_%d_%02d.bmp",
                             bdir, GetCurrentProcessId(), burst_no,
                             60 - s_burst_left);
                } else {
                    snprintf(path, sizeof(path), "fbburst_%d_%02d.bmp", burst_no,
                             60 - s_burst_left);
                }
                xbox_FramebufferDumpBmp(path);
                /* Which guest frame this image shows (F8 draw trace,
                 * docs Update 500). */
                fprintf(stderr, "  [FBWIN] burst %d image %02d = gpu frame %d\n",
                        burst_no, 60 - s_burst_left, nv2a_recomp_frame_time());
                s_burst_left--;
            }
        }
        {
            /* One dump, a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            static int frames, every;
            if (dump && strstr(dump, "%d")) {
                /* A "%d" in the path: one numbered dump every 600 refreshes
                 * (~10 s), to follow a sequence such as intro videos;
                 * RECOMP_FB_DUMP_EVERY=<refreshes> changes the interval. */
                if (!every) {
                    const char *ev = getenv("RECOMP_FB_DUMP_EVERY");
                    every = ev ? atoi(ev) : 0;
                    if (every < 1)
                        every = 600;
                }
                if (++frames % every == 0) {
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), dump, frames / every);
                    xbox_FramebufferDumpBmp(path);
                }
            } else if (dump && ++frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        /* Pacing (docs Update 480). Sleep(16) at the default ~15.6 ms timer
         * granularity slept ~31 ms -- the window's 32 refreshes a second
         * next to 60 guest flips. A GL present with vsync is paced by
         * SwapBuffers; everything else waits for the next 60 Hz deadline on
         * a high-resolution timer. */
        {
            static int vsync = -1;
            static HANDLE timer;
            static LARGE_INTEGER qf, next;
            LARGE_INTEGER now;
            if (vsync < 0) {
                const char *vs = getenv("RECOMP_VSYNC");
                vsync = !(vs && vs[0] == '0');
                timer = CreateWaitableTimerExW(NULL, NULL,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
                QueryPerformanceFrequency(&qf);
            }
            if (presented == 2) {
                /* Nothing new from the guest yet: look again in ~1 ms so a
                 * flip is shown promptly, not at the next 60 Hz slot. */
                LARGE_INTEGER due;
                due.QuadPart = -10000;
                if (timer) {
                    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                    WaitForSingleObject(timer, INFINITE);
                } else {
                    Sleep(1);
                }
            } else if (!(presented && vsync)) {
                LONGLONG period = qf.QuadPart / 60;
                QueryPerformanceCounter(&now);
                if (!next.QuadPart || now.QuadPart - next.QuadPart > period)
                    next.QuadPart = now.QuadPart;
                next.QuadPart += period;
                if (next.QuadPart > now.QuadPart) {
                    LONGLONG wait_100ns = (next.QuadPart - now.QuadPart) * 10000000 /
                                          qf.QuadPart;
                    if (timer) {
                        LARGE_INTEGER due;
                        due.QuadPart = -wait_100ns;
                        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                        WaitForSingleObject(timer, INFINITE);
                    } else {
                        Sleep((DWORD)(wait_100ns / 10000));
                    }
                }
            }
        }
    }

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSetSource(int (*fn)(uint32_t *, uint32_t, uint32_t)) { (void)fn; }
unsigned xbox_FramebufferBurstGeneration(void) { return 0; }
#endif
