/*
 * Framebuffer window for Linux hosts (SCDA ARM port, docs Update 677): the
 * SDL2 counterpart of fb_present.c's Win32 window, with the same entry
 * points and the same three picture sources -- the GPU's own presenter (an
 * EGL blit + swap into this X11 window, src/gpu/gloffscreen_egl.c), the
 * GPU's pixel source, or the guest-RAM scan-out.
 *
 * The window thread also owns keyboard and game-controller input: it feeds
 * key and pad state to the Win32 shim (w32_input_*), where the runtime reads
 * it with GetAsyncKeyState / XInputGetState as on Windows.
 */
#if !defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include "video_player.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_PinDeviceThread(void *h);
extern int nv2a_recomp_frame_time(void);

typedef int (*fb_host_source_fn)(uint32_t *dst, uint32_t width, uint32_t height);
typedef int (*fb_host_present_fn)(void *hwnd, uint32_t width, uint32_t height);

static volatile LONG s_fb_running;
static volatile LONG s_burst_generation;
static volatile LONG s_burst_request;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;
static uint32_t      s_out_w = 640, s_out_h = 480, s_img_w = 640, s_img_h = 480;
static volatile fb_host_source_fn s_host_source;
static volatile fb_host_present_fn s_host_present;
static HWND s_hwnd;          /* the X11 Window id */

void xbox_FramebufferWindowSetSource(fb_host_source_fn fn) { s_host_source = fn; }
void xbox_FramebufferWindowSetPresenter(fb_host_present_fn fn) { s_host_present = fn; }
void xbox_FramebufferBurstStart(void) { InterlockedExchange(&s_burst_request, 1); }
void *xbox_FramebufferWindowHandle(void) { return s_hwnd; }

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
    const char *pin = getenv("RECOMP_FB_VA");
    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    for (uint32_t y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;
        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (uint32_t x = 0; x < s_fb_width; x++) {
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

int xbox_FramebufferDumpBmp(const char *path)
{
    uint32_t row = ((s_img_w * 3u) + 3u) & ~3u;
    uint32_t img = row * s_img_h, total = 54u + img;
    uint8_t hdr[54], *line;
    FILE *f;

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
    for (uint32_t y = 0; y < s_img_h; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_img_h - 1 - y) * s_img_w;
        for (uint32_t x = 0; x < s_img_w; x++) {
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

/* ---- input ---------------------------------------------------------- */

static int scancode_to_vk(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return VK_F1 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return VK_NUMPAD1 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0:           return '0';
    case SDL_SCANCODE_KP_0:        return VK_NUMPAD0;
    case SDL_SCANCODE_KP_DIVIDE:   return VK_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
    case SDL_SCANCODE_KP_MINUS:    return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS:     return VK_ADD;
    case SDL_SCANCODE_KP_PERIOD:   return VK_DECIMAL;
    case SDL_SCANCODE_KP_ENTER:    return VK_RETURN;
    case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
    case SDL_SCANCODE_RETURN:      return VK_RETURN;
    case SDL_SCANCODE_BACKSPACE:   return VK_BACK;
    case SDL_SCANCODE_TAB:         return VK_TAB;
    case SDL_SCANCODE_ESCAPE:      return VK_ESCAPE;
    case SDL_SCANCODE_SPACE:       return VK_SPACE;
    case SDL_SCANCODE_PAGEUP:      return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN:    return VK_NEXT;
    case SDL_SCANCODE_END:         return VK_END;
    case SDL_SCANCODE_HOME:        return VK_HOME;
    case SDL_SCANCODE_LEFT:        return VK_LEFT;
    case SDL_SCANCODE_UP:          return VK_UP;
    case SDL_SCANCODE_RIGHT:       return VK_RIGHT;
    case SDL_SCANCODE_DOWN:        return VK_DOWN;
    case SDL_SCANCODE_INSERT:      return VK_INSERT;
    case SDL_SCANCODE_DELETE:      return VK_DELETE;
    case SDL_SCANCODE_PAUSE:       return VK_PAUSE;
    case SDL_SCANCODE_CAPSLOCK:    return VK_CAPITAL;
    case SDL_SCANCODE_SCROLLLOCK:  return VK_SCROLL;
    case SDL_SCANCODE_LSHIFT:      return VK_LSHIFT;
    case SDL_SCANCODE_RSHIFT:      return VK_RSHIFT;
    case SDL_SCANCODE_LCTRL:       return VK_LCONTROL;
    case SDL_SCANCODE_RCTRL:       return VK_RCONTROL;
    case SDL_SCANCODE_LALT:        return VK_LMENU;
    case SDL_SCANCODE_RALT:        return VK_RMENU;
    case SDL_SCANCODE_EQUALS:      return VK_OEM_PLUS;
    case SDL_SCANCODE_COMMA:       return VK_OEM_COMMA;
    case SDL_SCANCODE_MINUS:       return VK_OEM_MINUS;
    case SDL_SCANCODE_PERIOD:      return VK_OEM_PERIOD;
    default:                       return 0;
    }
}

#define FB_PADS 4
static SDL_GameController *s_pads[FB_PADS];
static LONG s_rumble_seen[FB_PADS];

static void pads_open(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < FB_PADS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int have = 0;
        for (int k = 0; k < FB_PADS; k++)
            if (s_pads[k] &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[k])) == id)
                have = 1;
        if (have)
            continue;
        while (slot < FB_PADS && s_pads[slot])
            slot++;
        if (slot >= FB_PADS)
            break;
        s_pads[slot] = SDL_GameControllerOpen(i);
        if (s_pads[slot])
            fprintf(stderr, "  [FBWIN] controller %d: %s\n", slot,
                    SDL_GameControllerName(s_pads[slot]));
    }
}

static void pads_close_removed(SDL_JoystickID id)
{
    for (int k = 0; k < FB_PADS; k++) {
        if (s_pads[k] &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[k])) == id) {
            SDL_GameControllerClose(s_pads[k]);
            s_pads[k] = NULL;
            w32_input_set_pad(k, NULL, 0);
            fprintf(stderr, "  [FBWIN] controller %d removed\n", k);
        }
    }
}

static SHORT stick(SDL_GameController *c, SDL_GameControllerAxis a, int invert)
{
    int v = SDL_GameControllerGetAxis(c, a);
    if (invert)
        v = -v - 1;
    return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void pads_poll(void)
{
    static const struct { SDL_GameControllerButton b; WORD x; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP,       XINPUT_GAMEPAD_DPAD_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     XINPUT_GAMEPAD_DPAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    XINPUT_GAMEPAD_DPAD_RIGHT },
        { SDL_CONTROLLER_BUTTON_START,         XINPUT_GAMEPAD_START },
        { SDL_CONTROLLER_BUTTON_BACK,          XINPUT_GAMEPAD_BACK },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK,     XINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  XINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_A,             XINPUT_GAMEPAD_A },
        { SDL_CONTROLLER_BUTTON_B,             XINPUT_GAMEPAD_B },
        { SDL_CONTROLLER_BUTTON_X,             XINPUT_GAMEPAD_X },
        { SDL_CONTROLLER_BUTTON_Y,             XINPUT_GAMEPAD_Y },
    };
    static XINPUT_STATE last[FB_PADS];
    for (int k = 0; k < FB_PADS; k++) {
        SDL_GameController *c = s_pads[k];
        XINPUT_STATE st;
        if (!c)
            continue;
        memset(&st, 0, sizeof(st));
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (SDL_GameControllerGetButton(c, map[i].b))
                st.Gamepad.wButtons |= map[i].x;
        st.Gamepad.bLeftTrigger =
            (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        st.Gamepad.bRightTrigger =
            (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        st.Gamepad.sThumbLX = stick(c, SDL_CONTROLLER_AXIS_LEFTX, 0);
        st.Gamepad.sThumbLY = stick(c, SDL_CONTROLLER_AXIS_LEFTY, 1);
        st.Gamepad.sThumbRX = stick(c, SDL_CONTROLLER_AXIS_RIGHTX, 0);
        st.Gamepad.sThumbRY = stick(c, SDL_CONTROLLER_AXIS_RIGHTY, 1);
        st.dwPacketNumber = last[k].dwPacketNumber;
        if (memcmp(&st.Gamepad, &last[k].Gamepad, sizeof(st.Gamepad)) != 0)
            st.dwPacketNumber++;
        last[k] = st;
        w32_input_set_pad(k, &st, 1);

        /* XInput motors run until changed; SDL rumble has a duration, so
         * renew it while it is on */
        {
            static Uint32 renewed[FB_PADS];
            XINPUT_VIBRATION v;
            LONG seq = w32_input_get_rumble(k, &v);
            Uint32 now = SDL_GetTicks();
            if (seq != s_rumble_seen[k] ||
                ((v.wLeftMotorSpeed || v.wRightMotorSpeed) && now - renewed[k] > 500)) {
                s_rumble_seen[k] = seq;
                renewed[k] = now;
                SDL_GameControllerRumble(c, v.wLeftMotorSpeed, v.wRightMotorSpeed, 1000);
            }
        }
    }
}

/* ---- window thread -------------------------------------------------- */

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && *v != '0';
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* the CPU picture sources, drawn through SDL's window surface */
static void blit_rgb(SDL_Window *win, uint32_t w, uint32_t h)
{
    SDL_Surface *dst = SDL_GetWindowSurface(win);
    SDL_Surface *src;
    if (!dst)
        return;
    src = SDL_CreateRGBSurfaceWithFormatFrom(s_rgb, (int)w, (int)h, 32, (int)w * 4,
                                             SDL_PIXELFORMAT_RGB888);
    if (!src)
        return;
    SDL_BlitScaled(src, NULL, dst, NULL);
    SDL_FreeSurface(src);
    SDL_UpdateWindowSurface(win);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    SDL_Window *win;
    SDL_SysWMinfo wm;
    int gl_presenting = 0;
    (void)unused;

    xbox_PinDeviceThread(GetCurrentThread());

    /* the presenter draws with EGL on an X11 window */
    if (!getenv("SDL_VIDEODRIVER"))
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* the CPU picture path draws through a plain (XShm) window surface: an
     * accelerated one is a GLX context on the window, which the EGL
     * presenter's surface then cannot share (BadAccess in glXMakeCurrent) */
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  [FBWIN] SDL_Init failed: %s\n", SDL_GetError());
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    {
        const char *sc = getenv("RECOMP_SURFACE_SCALE");
        int scale = sc ? atoi(sc) : 1;
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;
        s_out_w = s_fb_width * (uint32_t)scale;
        s_out_h = s_fb_height * (uint32_t)scale;
        if (env_on("RECOMP_WIDESCREEN"))
            s_out_w = s_out_h * 16u / 9u;
    }
    win = SDL_CreateWindow("Xbox Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           (int)s_out_w, (int)s_out_h,
                           SDL_WINDOW_RESIZABLE |
                           (env_on("RECOMP_FULLSCREEN") ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) {
        fprintf(stderr, "  [FBWIN] SDL_CreateWindow failed: %s\n", SDL_GetError());
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_X11)
        s_hwnd = (HWND)(uintptr_t)wm.info.x11.window;
    else
        fprintf(stderr, "  [FBWIN] not an X11 window: GPU presenting disabled\n");
    w32_set_foreground_window(s_hwnd ? s_hwnd : (HWND)win);
    if (SDL_GetModState() & KMOD_NUM) {
        w32_input_set_key(VK_NUMLOCK, 1);   /* toggled on */
        w32_input_set_key(VK_NUMLOCK, 0);
    }
    pads_open();

    s_rgb = (uint32_t *)calloc((size_t)s_out_w * s_out_h, 4);
    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s)\n",
            s_out_w, s_out_h, SDL_GetCurrentVideoDriver());

    uint64_t next = 0, t_title = 0, t_rate = 0, last_frame = 0;
    unsigned shown = 0, shown_title = 0;
    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
                    fprintf(stderr, "  [FBWIN] window closed by the user: ending the program\n");
                    fflush(NULL);
                    _exit(0);
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (!ev.key.repeat)
                    w32_input_set_key(scancode_to_vk(ev.key.keysym.scancode),
                                      ev.type == SDL_KEYDOWN);
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    w32_set_foreground_window(s_hwnd ? s_hwnd : (HWND)win);
                else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST &&
                         !env_on("RECOMP_INPUT_BACKGROUND"))
                    w32_set_foreground_window(NULL);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                pads_open();
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                pads_close_removed(ev.cdevice.which);
                break;
            default:
                break;
            }
        }
        pads_poll();

        int presented = 0, w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        if (s_hwnd && s_host_present && !getenv("RECOMP_FB_DUMP") && w > 0 && h > 0)
            presented = s_host_present(s_hwnd, (uint32_t)w, (uint32_t)h);
        if (presented)
            gl_presenting = 1;
        if (presented == 1) {
            uint64_t t = now_ns();
            if (last_frame && shown > 300 && t - last_frame > 40000000ull)
                fprintf(stderr, "[FRAME-HITCH] %.1f ms frame at tick %lu\n",
                        (double)(t - last_frame) / 1e6, (unsigned long)GetTickCount());
            last_frame = t;
            shown++;
            shown_title++;
        } else if (!presented && !gl_presenting && s_host_source && s_rgb &&
                   s_host_source(s_rgb, s_out_w, s_out_h)) {
            s_img_w = s_out_w;
            s_img_h = s_out_h;
            blit_rgb(win, s_img_w, s_img_h);
            shown++;
            shown_title++;
        } else if (!presented && !gl_presenting && s_fb_va && s_fb_pitch && s_rgb) {
            fb_convert((const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset()),
                       s_fb_pitch / s_fb_width);
            s_img_w = s_fb_width;
            s_img_h = s_fb_height;
            blit_rgb(win, s_img_w, s_img_h);
            shown++;
            shown_title++;
        }
        {
            uint64_t t = now_ns();
            if (!t_title)
                t_title = t;
            if (t - t_title >= 1000000000ull) {
                char title[96];
                snprintf(title, sizeof(title), "Xbox Recomp - %u FPS",
                         (unsigned)((shown_title * 1000000000ull + (t - t_title) / 2) /
                                    (t - t_title)));
                SDL_SetWindowTitle(win, title);
                shown_title = 0;
                t_title = t;
            }
            if (!t_rate)
                t_rate = t;
            if (t - t_rate >= 5000000000ull) {
                fprintf(stderr, "[FBWIN-RATE] %u window refreshes in %llu ms (%s)\n",
                        shown, (unsigned long long)((t - t_rate) / 1000000ull),
                        gl_presenting ? "GL present" : "CPU");
                shown = 0;
                t_rate = t;
            }
        }
        {
            /* RECOMP_FB_DUMP as on Windows: one dump after 600 refreshes, or
             * every RECOMP_FB_DUMP_EVERY with a "%d" in the path */
            const char *dump = getenv("RECOMP_FB_DUMP");
            static int frames, every;
            if (dump && strstr(dump, "%d")) {
                if (!every) {
                    const char *e = getenv("RECOMP_FB_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
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
            if (InterlockedExchange(&s_burst_request, 0))
                InterlockedIncrement(&s_burst_generation);
        }
        {
            /* RECOMP_FB_SHOT_AT="30,60,90" with RECOMP_FB_SHOT=<path with %d>:
             * single dumps at those seconds after the window opened, read
             * from the GPU's pixel source, so the GL presenter keeps running
             * (RECOMP_FB_DUMP switches it off for the whole run) */
            static uint64_t t_open;
            static const char *at;
            static int shot_no;
            const char *shot = getenv("RECOMP_FB_SHOT");
            if (!t_open) {
                t_open = now_ns();
                at = getenv("RECOMP_FB_SHOT_AT");
            }
            if (shot && at && *at && s_host_source && s_rgb &&
                (now_ns() - t_open) / 1000000000ull >= strtoull(at, NULL, 10)) {
                char path[MAX_PATH];
                const char *comma = strchr(at, ',');
                at = comma ? comma + 1 : "";
                if (s_host_source(s_rgb, s_out_w, s_out_h)) {
                    s_img_w = s_out_w;
                    s_img_h = s_out_h;
                    snprintf(path, sizeof(path), shot, ++shot_no);
                    xbox_FramebufferDumpBmp(path);
                }
            }
        }
        /* pacing: a vsynced GL present waits in the swap; otherwise 60 Hz */
        {
            static int vsync = -1;
            if (vsync < 0) {
                const char *vs = getenv("RECOMP_VSYNC");
                vsync = !(vs && vs[0] == '0');
            }
            if (presented == 2) {
                struct timespec ts = { 0, 1000000 };
                nanosleep(&ts, NULL);
            } else if (!(presented && vsync)) {
                const uint64_t period = 1000000000ull / 60;
                uint64_t t = now_ns();
                if (!next || t - next > period)
                    next = t;
                next += period;
                if (next > t) {
                    struct timespec ts = { (time_t)((next - t) / 1000000000ull),
                                           (long)((next - t) % 1000000000ull) };
                    nanosleep(&ts, NULL);
                }
            }
        }
    }
    SDL_DestroyWindow(win);
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
#endif /* !_WIN32 */
