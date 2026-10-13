/*
 * scda_options.txt next to the executable (docs Updates 614-615): the
 * player's settings in one plain text file instead of `set RECOMP_...` lines
 * in launcher batch files. Written with defaults and explanations when
 * missing. Each known key becomes the RECOMP_* environment variable the
 * runtime already reads -- unless that variable is set already, so the
 * diagnostic launchers keep overriding single settings. Lines starting with
 * '#' are comments. RECOMP_NO_OPTIONS=1 skips the file entirely.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scda_options.h"

#define OPTIONS_FILE "scda_options.txt"

static const char s_template[] =
"# SCDA Recomp - settings\r\n"
"# This file sits next to scda_recomp.exe and is read when the game starts.\r\n"
"# Changes take effect on the next start. Lines starting with # are comments.\r\n"
"# If the file is missing, it is recreated with these defaults.\r\n"
"# Values: 0 = off, 1 = on, unless stated otherwise.\r\n"
"\r\n"
"[Audio]\r\n"
"# Surround: 0 = stereo through Windows\r\n"
"#           1 = Dolby Digital 5.1 passed straight to an AV receiver\r\n"
"#               (optical/coaxial S/PDIF or HDMI). The Windows volume has no\r\n"
"#               effect then; set the volume on the receiver. Falls back to 2\r\n"
"#               if the output does not accept Dolby Digital.\r\n"
"#           2 = 5.1 as multichannel PCM through Windows (5.1 speakers on\r\n"
"#               HDMI/analog; a stereo device gets a stereo downmix)\r\n"
"Surround = 0\r\n"
"# AudioDevice: empty = Windows default output, otherwise part of the\r\n"
"# device name, e.g. AudioDevice = S/PDIF\r\n"
"AudioDevice =\r\n"
"# MuteInBackground: 1 = silent while the game window is not in front\r\n"
"# (with Surround = 1 the receiver briefly switches to PCM)\r\n"
"MuteInBackground = 0\r\n"
"\r\n"
"[Graphics]\r\n"
"# RenderScale: internal resolution, 1 to 4 times 640x480\r\n"
"RenderScale = 2\r\n"
"# Widescreen: 1 = 16:9, 0 = 4:3\r\n"
"Widescreen = 1\r\n"
"# FPS60: 1 = 60 frames per second, 0 = 30 as on the Xbox\r\n"
"# (videos always play at their own speed)\r\n"
"FPS60 = 1\r\n"
"# FPS60Coop: split-screen co-op at 60 (1) or at 30 as on the Xbox (0);\r\n"
"# only with FPS60 = 1.\r\n"
"FPS60Coop = 1\r\n"
"# BloomSmoothing: 0-3, smooths what the glow effect samples at RenderScale > 1.\r\n"
"#   0 = off (small lights flicker in the glow), 2 = default, 3 = steadier, wider glow\r\n"
"BloomSmoothing = 2\r\n"
"# AntiAliasing: 1 = smooth jagged edges of the finished picture, 0 = off\r\n"
"AntiAliasing = 1\r\n"
"# VSync: 1 = synchronise to the monitor (no tearing)\r\n"
"VSync = 1\r\n"
"# Renderer: vulkan or opengl\r\n"
"Renderer = vulkan\r\n"
"\r\n"
"[Window]\r\n"
"# Fullscreen: 1 = borderless fullscreen, 0 = window\r\n"
"Fullscreen = 0\r\n"
"# Monitor: screen the game starts on, 1 = primary, 2 = second, ...\r\n"
"Monitor = 1\r\n"
"# RememberWindow: 1 = keep window size and position when closing\r\n"
"# (window mode only; stored in scda_window.txt)\r\n"
"RememberWindow = 1\r\n"
"\r\n"
"[Game]\r\n"
"# Language: EN, DE, FR, ES, IT or JA\r\n"
"Language = EN\r\n"
"# SkipIntro: 1 = skip the logo and intro videos at start-up\r\n"
"SkipIntro = 0\r\n"
"# Keyboard: 1 = keyboard in addition to a controller\r\n"
"#   Player 1: Enter = START, Backspace = BACK, Home/Delete/End/PageDown =\r\n"
"#   D-pad, A/B/X/Y = A/B/X/Y, E/S/D/F = left stick, I/J/K/L = right stick,\r\n"
"#   W/O = left/right trigger, 1/2 = White/Black, 3/4 = stick clicks\r\n"
"Keyboard = 1\r\n"
"# Keyboard2: 1 = second keyboard controller (player 2, co-op), NumLock on:\r\n"
"#   arrow keys = left stick, numpad 8/4/2/6 = right stick, numpad 7/1/9/3 =\r\n"
"#   A/B/X/Y, numpad + = LB, numpad , = RB, numpad 0 = START, numpad / =\r\n"
"#   BACK, numpad -/* = left/right trigger, numpad 5 = left stick click.\r\n"
"#   0 = arrow keys are player 1's D-pad again\r\n"
"Keyboard2 = 1\r\n";

typedef struct {
    const char *key;      /* in the file, case-insensitive */
    const char *alias;    /* the German name of the first version (Update 614) */
    const char *env;      /* RECOMP_* variable it sets */
} OptionMap;

static const OptionMap s_map[] = {
    { "Surround",         NULL,                 "RECOMP_AUDIO_SURROUND" },
    { "AudioDevice",      "Audiogeraet",        "RECOMP_AUDIO_DEVICE" },
    { "MuteInBackground", "HintergrundStumm",   "RECOMP_MUTE_IN_BACKGROUND" },
    { "RenderScale",      "Aufloesung",         "RECOMP_SURFACE_SCALE" },
    { "Widescreen",       "Breitbild",          "RECOMP_WIDESCREEN" },
    { "FPS60",            NULL,                 "RECOMP_60FPS" },
    { "FPS60Coop",        NULL,                 "RECOMP_60FPS_COOP" },
    { "BloomSmoothing",   NULL,                 "RECOMP_VK_BLOOM_PREFILTER" },
    { "AntiAliasing",     NULL,                 "RECOMP_FXAA" },
    { "VSync",            NULL,                 "RECOMP_VSYNC" },
    { "Renderer",         NULL,                 "RECOMP_RENDERER" },
    { "Fullscreen",       "Vollbild",           "RECOMP_FULLSCREEN" },
    { "Monitor",          NULL,                 "RECOMP_MONITOR" },
    { "RememberWindow",   "FensterMerken",      "RECOMP_REMEMBER_WINDOW" },
    { "Language",         "Sprache",            "RECOMP_XC_LANGUAGE" },
    { "SkipIntro",        "IntroUeberspringen", "RECOMP_SKIP_INTRO" },
    { "Keyboard",         "Tastatur",           "RECOMP_INPUT_KEYBOARD" },
    { "Keyboard2",        NULL,                 "RECOMP_INPUT_KEYBOARD2" },
};

/* What the normal launcher (start_scda_xemu_vulkan.bat) always set and the
 * player never needs to touch: xemu's GPU in the fast mode, its CPU fast
 * paths (docs Update 595-596), the APU/AC'97 handshake and the game window. */
static const char *const s_fixed[][2] = {
    { "RECOMP_NV2A_XEMU", "1" },
    { "RECOMP_NV2A_TCG", "1" },
    { "RECOMP_VK_TEXTURE_CLEAN_FAST", "1" },
    { "RECOMP_VK_UNIFORM_BULK_COPY", "1" },
    { "RECOMP_AC97_READY", "1" },
    { "RECOMP_FB_WINDOW", "1" },
    /* JBA paper meshes lie 1-16 depth steps above the concrete and z-fight
     * there, on real hardware too; the user wanted that fixed. Pull them
     * towards the camera, recognised by their vertex data (docs Update
     * 634); the signatures survive the level re-streaming the meshes. */
    { "RECOMP_ZBIAS_MESHES", "E7C4ADB5+C3150245:-256:-8" },
    /* Shadow maps (Z16 render target smaller than the 512x512 texture
     * reading it) stay on the GPU instead of eight guest-RAM round trips
     * per frame in JBA: 15 -> 8 readbacks, ~43 -> ~47 FPS at 3x, shadows
     * pixel-identical (docs Update 635). */
    { "RECOMP_VK_Z16_PARTIAL_TEX", "1" },
    /* Shadow maps and the bloom chain share 04C99000..: no readback/upload
     * when that memory changes between depth and colour use, none before a
     * fully Z-cleared larger shadow map, and the frame copy for the post
     * effects stays on the GPU (docs Update 638): 9 -> 1 readbacks per
     * frame at the user's JBA save, laser beams the only image difference. */
    { "RECOMP_VK_CROSS_TYPE_DISCARD", "1" },
    { "RECOMP_VK_Z16_FULL_CLEAR_REUSE", "1" },
    { "RECOMP_VK_SCDA_LAZY_BLIT", "1" },
};

static void set_default(const char *env, const char *value)
{
    const char *cur = getenv(env);
    if (cur && *cur)
        return;            /* a launcher's explicit setting wins */
    _putenv_s(env, value);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* The file's values in the runtime's encoding. NULL = invalid, skipped. */
static const char *translate(const char *env, const char *v)
{
    if (!strcmp(env, "RECOMP_AUDIO_SURROUND")) {
        if (!strcmp(v, "0")) return "0";
        if (!strcmp(v, "1")) return "passthrough";
        if (!strcmp(v, "2")) return "1";
        return NULL;
    }
    if (!strcmp(env, "RECOMP_XC_LANGUAGE")) {
        /* The console's XC_LANGUAGE values. */
        static const char *const codes[][2] = {
            { "EN", "1" }, { "JA", "2" }, { "DE", "3" }, { "FR", "4" },
            { "ES", "5" }, { "IT", "6" }, { "KO", "7" }, { "ZH", "8" },
            { "PT", "9" },
            /* the first version's German names */
            { "englisch", "1" }, { "deutsch", "3" }, { "franzoesisch", "4" },
            { "spanisch", "5" }, { "italienisch", "6" }, { "japanisch", "2" },
        };
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
            if (!_stricmp(v, codes[i][0]))
                return codes[i][1];
        return (v[0] >= '1' && v[0] <= '9' && !v[1]) ? v : NULL;
    }
    if (!strcmp(env, "RECOMP_SURFACE_SCALE"))
        return (v[0] >= '1' && v[0] <= '4' && !v[1]) ? v : NULL;
    if (!strcmp(env, "RECOMP_RENDERER"))
        return (!_stricmp(v, "vulkan") || !_stricmp(v, "opengl")) ? v : NULL;
    return v;
}

void scda_options_load(void)
{
    char path[MAX_PATH], line[512];
    const char *no = getenv("RECOMP_NO_OPTIONS");
    DWORD n;
    FILE *f;
    int applied = 0;

    if (no && *no && *no != '0')
        return;
    n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\')
        n--;
    if (!n || n + sizeof(OPTIONS_FILE) >= MAX_PATH)
        return;
    strcpy_s(path + n, MAX_PATH - n, OPTIONS_FILE);

    if (fopen_s(&f, path, "rb") != 0 || !f) {
        if (fopen_s(&f, path, "wb") == 0 && f) {
            fwrite(s_template, 1, sizeof(s_template) - 1, f);
            fclose(f);
        }
        if (fopen_s(&f, path, "rb") != 0 || !f)
            f = NULL;
    }
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *s = trim(line), *eq, *key, *val;
            const char *out;
            if (!*s || *s == '#' || *s == ';' || *s == '[')
                continue;
            eq = strchr(s, '=');
            if (!eq)
                continue;
            *eq = '\0';
            key = trim(s);
            val = trim(eq + 1);
            {
                char *c = strchr(val, '#');   /* trailing comment */
                if (c) { *c = '\0'; val = trim(val); }
            }
            for (size_t i = 0; i < sizeof(s_map) / sizeof(s_map[0]); i++) {
                if (_stricmp(key, s_map[i].key) &&
                    !(s_map[i].alias && !_stricmp(key, s_map[i].alias)))
                    continue;
                if (!*val)
                    break;                 /* empty = not set */
                out = translate(s_map[i].env, val);
                if (out) {
                    set_default(s_map[i].env, out);
                    applied++;
                } else {
                    fprintf(stderr, "[OPTIONS] %s: invalid value \"%s\"\n", key, val);
                }
                break;
            }
        }
        fclose(f);
    }
    for (size_t i = 0; i < sizeof(s_fixed) / sizeof(s_fixed[0]); i++)
        set_default(s_fixed[i][0], s_fixed[i][1]);
    fprintf(stderr, "[OPTIONS] %s: %d settings%s\n", path, applied,
            f ? "" : " (file not readable; defaults)");
}
