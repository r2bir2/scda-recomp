/*
 * Host side of SCDA's controller bridge (docs Update 347).
 *
 * SCDA links the Xbox XAPI input stack statically (section XPP). Its USB host
 * controller / XID driver never runs under the recomp, so the device tables
 * stay empty and the title waits forever on "Press START". recomp_manual.c
 * replaces the public XAPI entry points (XGetDevices, XGetDeviceChanges,
 * XInputOpen/Close/GetState/SetState) and asks this file for the host state.
 *
 * Ports 0 and 1 are virtual pads that merge a keyboard layout with the
 * first/second Windows XInput controller, so both are always connected (the
 * second one enables co-op, docs Update 648); ports 2-3 follow the XInput
 * controllers 2-3. RECOMP_INPUT_KEYBOARD=0 turns the keyboard off (ports 0
 * and 1 then follow their XInput pads like the others);
 * RECOMP_INPUT_KEYBOARD2=0 turns off only the second keyboard pad.
 *
 * Keyboard (only while a window of this process has the focus):
 *   player 1: Enter = START, Backspace = BACK,
 *   Home/Delete/End/PageDown = D-pad up/left/down/right (the arrow keys when
 *   the second keyboard pad is off), A/B/X/Y keys = A/B/X/Y, 1 = White,
 *   2 = Black, 3/4 = left/right stick click, E/S/D/F = left stick
 *   up/left/down/right, I/J/K/L = right stick, W = left trigger,
 *   O = right trigger. F12 = dump guest RAM (see dump_guest_ram).
 *   player 2 (needs NumLock on): arrow keys = left stick, numpad 8/4/2/6 =
 *   right stick, numpad 7/1/9/3 = A/B/X/Y, numpad + = White (LB),
 *   numpad , = Black (RB), numpad 0 = START, numpad / = BACK,
 *   numpad - = left trigger, numpad * = right trigger, numpad 5 = left
 *   stick click.
 *
 * RECOMP_INPUT_SCRIPT (see script_load) presses buttons on port 0 at fixed
 * times for unattended tests.
 *
 * The Xbox XINPUT_GAMEPAD the title reads is 18 bytes: WORD wButtons,
 * BYTE bAnalogButtons[8] (A B X Y Black White LT RT, 0-255), then SHORT
 * sThumbLX, sThumbLY, sThumbRX, sThumbRY.
 */

#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#pragma comment(lib, "xinput.lib")

#define PAD_PORTS 4

/* XInput polls of an empty slot are slow (a device enumeration each time),
 * so a disconnected slot is re-checked at most every 500 ms. */
static DWORD g_retry_tick[PAD_PORTS];

static int keyboard_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("RECOMP_INPUT_KEYBOARD");
        cached = !(e && e[0] == '0');
    }
    return cached;
}

/* Second keyboard pad on port 1 (docs Update 648: co-op needs a second
 * connected controller). */
static int keyboard2_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("RECOMP_INPUT_KEYBOARD2");
        cached = keyboard_enabled() && !(e && e[0] == '0');
    }
    return cached;
}

static int keyboard_port(unsigned port)
{
    return (port == 0 && keyboard_enabled()) || (port == 1 && keyboard2_enabled());
}

static int xinput_read(unsigned port, XINPUT_STATE *st)
{
    DWORD now = GetTickCount();
    if (g_retry_tick[port] && (int32_t)(now - g_retry_tick[port]) < 0)
        return 0;
    if (XInputGetState(port, st) != ERROR_SUCCESS) {
        g_retry_tick[port] = now + 500;
        return 0;
    }
    g_retry_tick[port] = 0;
    return 1;
}

static int our_window_has_focus(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int scda_input_port_connected(unsigned port)
{
    XINPUT_STATE st;
    if (port >= PAD_PORTS)
        return 0;
    if (keyboard_port(port))
        return 1;
    return xinput_read(port, &st);
}

static int16_t axis(int neg, int pos)
{
    return (int16_t)(pos ? 32767 : 0) - (int16_t)(neg ? 32767 : 0);
}

static int16_t merge_axis(int16_t pad, int16_t kb)
{
    return kb ? kb : pad;
}

/* RECOMP_INPUT_SCRIPT="45=START,60=A,62=DOWN": unattended test presses on
 * port 0, each held for 1 s, at seconds after the first state poll.
 * RSLEFT/RSRIGHT/RSUP/RSDOWN drive the right stick for reproducible
 * camera-angle performance probes, independent of window focus. */
#define SCRIPT_MAX 32
static struct { DWORD at_ms; uint16_t buttons; int analog; DWORD hold_ms; } g_script[SCRIPT_MAX];
static int g_script_n = -1;
static DWORD g_script_t0;

static void script_load(void)
{
    static const struct { const char *name; uint16_t buttons; int analog; } names[] = {
        {"UP", 0x01, -1}, {"DOWN", 0x02, -1}, {"LEFT", 0x04, -1}, {"RIGHT", 0x08, -1},
        {"START", 0x10, -1}, {"BACK", 0x20, -1},
        {"A", 0, 0}, {"B", 0, 1}, {"X", 0, 2}, {"Y", 0, 3},
        {"RSLEFT", 0, -2}, {"RSRIGHT", 0, -3},
        {"RSUP", 0, -4}, {"RSDOWN", 0, -5},
        /* left stick (docs Update 624): walk Sam in automated runs */
        {"LSLEFT", 0, -6}, {"LSRIGHT", 0, -7},
        {"LSUP", 0, -8}, {"LSDOWN", 0, -9},
    };
    const char *s = getenv("RECOMP_INPUT_SCRIPT");
    g_script_n = 0;
    g_script_t0 = GetTickCount();
    /* RECOMP_INPUT_SCRIPT_CLOCK=process (docs Update 629): count the script
     * times from process start, like RECOMP_TRACE_BURST_AT, instead of from
     * the title's first pad poll. */
    {
        const char *clk = getenv("RECOMP_INPUT_SCRIPT_CLOCK");
        FILETIME c, e, k, u, now;
        if (clk && !strcmp(clk, "process") &&
            GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            ULARGE_INTEGER a, b;
            GetSystemTimeAsFileTime(&now);
            a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
            b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
            g_script_t0 -= (DWORD)((b.QuadPart - a.QuadPart) / 10000);
        }
        fprintf(stderr, "[SCDA-INPUT] script loaded, clock %s, %u ms since its zero\n",
                clk ? clk : "first-poll", (unsigned)(GetTickCount() - g_script_t0));
    }
    while (s && *s && g_script_n < SCRIPT_MAX) {
        char name[16];
        unsigned sec = 0, k, i = 0;
        while (*s >= '0' && *s <= '9')
            sec = sec * 10 + (unsigned)(*s++ - '0');
        if (*s++ != '=')
            break;
        DWORD hold = 1000;
        while (*s && *s != ',' && *s != ':' && i < sizeof name - 1)
            name[i++] = *s++;
        name[i] = 0;
        /* "NAME:ms" holds for ms instead of 1 s (docs Update 646: small
         * camera moves for performance probes). */
        if (*s == ':') {
            s++;
            hold = 0;
            while (*s >= '0' && *s <= '9')
                hold = hold * 10 + (DWORD)(*s++ - '0');
        }
        while (*s && *s != ',')
            s++;
        if (*s == ',')
            s++;
        for (k = 0; k < sizeof names / sizeof names[0]; k++)
            if (!strcmp(name, names[k].name)) {
                g_script[g_script_n].at_ms = sec * 1000u;
                g_script[g_script_n].buttons = names[k].buttons;
                g_script[g_script_n].analog = names[k].analog;
                g_script[g_script_n].hold_ms = hold;
                g_script_n++;
                break;
            }
    }
}

static void script_apply(uint16_t *buttons, uint8_t *analog,
                         int16_t *rx, int16_t *ry, int16_t *lx, int16_t *ly)
{
    DWORD t;
    int i;
    if (g_script_n < 0)
        script_load();
    t = GetTickCount() - g_script_t0;
    for (i = 0; i < g_script_n; i++)
        if (t >= g_script[i].at_ms && t < g_script[i].at_ms + g_script[i].hold_ms) {
            *buttons |= g_script[i].buttons;
            if (g_script[i].analog >= 0)
                analog[g_script[i].analog] = 255;
            else if (g_script[i].analog == -2)
                *rx = -32767;
            else if (g_script[i].analog == -3)
                *rx = 32767;
            else if (g_script[i].analog == -4)
                *ry = 32767;
            else if (g_script[i].analog == -5)
                *ry = -32767;
            else if (g_script[i].analog == -6)
                *lx = -32767;
            else if (g_script[i].analog == -7)
                *lx = 32767;
            else if (g_script[i].analog == -8)
                *ly = 32767;
            else if (g_script[i].analog == -9)
                *ly = -32767;
        }
}

/* F12 (window focused): write the guest's 64 MB of RAM to ram_dump_<tick>.bin
 * in the working directory. On-screen text is unreadable while the font
 * rendering is broken, but the strings a message box shows sit in guest
 * memory as UTF-16 (docs Update 348). Unreadable pages are written as 0. */
extern ptrdiff_t g_xbox_mem_offset;

void xbox_coverage_toggle(void);  /* src/kernel/recomp_coverage.c */
void scda_sample_all_now(void);   /* main.c */

static void dump_guest_ram(void)
{
    char name[64];
    HANDLE f;
    uint32_t va;
    static uint8_t zero[4096];

    snprintf(name, sizeof name, "ram_dump_%lu.bin", (unsigned long)GetTickCount());
    f = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    for (va = 0; va < 64u * 1024u * 1024u; va += 4096u) {
        const uint8_t *p = (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
        MEMORY_BASIC_INFORMATION mbi;
        DWORD done;
        int ok = VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                 !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        WriteFile(f, ok ? p : zero, 4096, &done, NULL);
    }
    CloseHandle(f);
    fprintf(stderr, "[SCDA-INPUT] F12: guest RAM written to %s\n", name);
}

/* Fills out[18] with the Xbox gamepad report for port. Returns 0 when the
 * port has no device. */
int scda_input_get_state(unsigned port, uint8_t *out)
{
    XINPUT_STATE st;
    uint16_t buttons = 0;
    uint8_t analog[8] = {0};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int have_pad, have_kb;

    if (port >= PAD_PORTS)
        return 0;
    have_pad = xinput_read(port, &st);
    have_kb = keyboard_port(port);
    if (!have_pad && !have_kb)
        return 0;

    if (have_pad) {
        WORD b = st.Gamepad.wButtons;
        buttons = b & 0x00FF;  /* D-pad, START, BACK, stick clicks: same bits */
        analog[0] = (b & XINPUT_GAMEPAD_A) ? 255 : 0;
        analog[1] = (b & XINPUT_GAMEPAD_B) ? 255 : 0;
        analog[2] = (b & XINPUT_GAMEPAD_X) ? 255 : 0;
        analog[3] = (b & XINPUT_GAMEPAD_Y) ? 255 : 0;
        analog[4] = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;  /* Black */
        analog[5] = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;   /* White */
        analog[6] = st.Gamepad.bLeftTrigger;
        analog[7] = st.Gamepad.bRightTrigger;
        lx = st.Gamepad.sThumbLX;
        ly = st.Gamepad.sThumbLY;
        rx = st.Gamepad.sThumbRX;
        ry = st.Gamepad.sThumbRY;
    }

    if (have_kb && port == 1 && our_window_has_focus()) {
        static int numlock_warned;
        if (!numlock_warned && !(GetKeyState(VK_NUMLOCK) & 1)) {
            /* Without NumLock the numpad sends arrow/Home/End codes, which
             * are player 2's left stick and player 1's D-pad. */
            numlock_warned = 1;
            fprintf(stderr, "[SCDA-INPUT] NumLock is off: player 2's numpad keys "
                    "need NumLock on\n");
        }
        if (key(VK_NUMPAD0))  buttons |= 0x0010;
        if (key(VK_DIVIDE))   buttons |= 0x0020;
        if (key(VK_NUMPAD5))  buttons |= 0x0040;
        if (key(VK_NUMPAD7))  analog[0] = 255;
        if (key(VK_NUMPAD1))  analog[1] = 255;
        if (key(VK_NUMPAD9))  analog[2] = 255;
        if (key(VK_NUMPAD3))  analog[3] = 255;
        if (key(VK_DECIMAL))  analog[4] = 255;
        if (key(VK_ADD))      analog[5] = 255;
        if (key(VK_SUBTRACT)) analog[6] = 255;
        if (key(VK_MULTIPLY)) analog[7] = 255;
        lx = merge_axis(lx, axis(key(VK_LEFT), key(VK_RIGHT)));
        ly = merge_axis(ly, axis(key(VK_DOWN), key(VK_UP)));
        rx = merge_axis(rx, axis(key(VK_NUMPAD4), key(VK_NUMPAD6)));
        ry = merge_axis(ry, axis(key(VK_NUMPAD2), key(VK_NUMPAD8)));
    }

    if (have_kb && port == 0 && our_window_has_focus()) {
        /* The arrow keys belong to player 2 while that pad is on. */
        int p2 = keyboard2_enabled();
        static int f12_was_down, f10_was_down, f9_was_down;
        int f12 = key(VK_F12);
        int f10 = key(VK_F10);
        int f9 = key(VK_F9);
        /* F9: 10 s performance sample of all threads (main.c). */
        if (f9 && !f9_was_down)
            scda_sample_all_now();
        f9_was_down = f9;
        if (f12 && !f12_was_down)
            dump_guest_ram();
        f12_was_down = f12;
        /* F10: start/stop basic-block coverage recording (docs Update 460),
         * for diffing a working and a broken run of the same scene. */
        if (f10 && !f10_was_down)
            xbox_coverage_toggle();
        f10_was_down = f10;
        if (key(p2 ? VK_HOME : VK_UP))      buttons |= 0x0001;
        if (key(p2 ? VK_END : VK_DOWN))     buttons |= 0x0002;
        if (key(p2 ? VK_DELETE : VK_LEFT))  buttons |= 0x0004;
        if (key(p2 ? VK_NEXT : VK_RIGHT))   buttons |= 0x0008;
        if (key(VK_RETURN)) buttons |= 0x0010;
        if (key(VK_BACK))   buttons |= 0x0020;
        if (key('3'))       buttons |= 0x0040;
        if (key('4'))       buttons |= 0x0080;
        if (key('A')) analog[0] = 255;
        if (key('B')) analog[1] = 255;
        if (key('X')) analog[2] = 255;
        if (key('Y')) analog[3] = 255;
        if (key('2')) analog[4] = 255;
        if (key('1')) analog[5] = 255;
        if (key('W')) analog[6] = 255;
        if (key('O')) analog[7] = 255;
        lx = merge_axis(lx, axis(key('S'), key('F')));
        ly = merge_axis(ly, axis(key('D'), key('E')));
        rx = merge_axis(rx, axis(key('J'), key('L')));
        ry = merge_axis(ry, axis(key('K'), key('I')));
    }
    if (port == 0)
        script_apply(&buttons, analog, &rx, &ry, &lx, &ly);

    memcpy(out + 0, &buttons, 2);
    memcpy(out + 2, analog, 8);
    memcpy(out + 10, &lx, 2);
    memcpy(out + 12, &ly, 2);
    memcpy(out + 14, &rx, 2);
    memcpy(out + 16, &ry, 2);
    return 1;
}

void scda_input_rumble(unsigned port, uint16_t left, uint16_t right)
{
    XINPUT_VIBRATION v;
    if (port >= PAD_PORTS)
        return;
    v.wLeftMotorSpeed = left;
    v.wRightMotorSpeed = right;
    XInputSetState(port, &v);
}
