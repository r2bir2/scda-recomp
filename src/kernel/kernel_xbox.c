/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#include "kernel.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* src/apu/apu_surround.c: 1 while the opt-in surround path runs. */
extern int mcpx_apu_surround_active(void);

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - default report is a standard retail Xbox.
 * A target may override via xbox_kernel_set_version() or by patching the
 * global directly; no title-specific values are baked in. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - default is a stock retail value. Titles built against a
 * different XDK can report their own through xbox_kernel_set_version(). */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Override the kernel version reported to game code (ordinal 324).
 * Match the XDK revision the target title was built with if it checks it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof(g_image_filename) - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region: North America
 *   - Video: NTSC, widescreen+HDTV enabled
 *   - Language: English
 *   - Audio: Stereo, no Dolby Digital (as xemu's default EEPROM)
 *   - DVD Region: Region 1 (North America)
 * ============================================================================ */

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        /* English = 1. RECOMP_XC_LANGUAGE=<n> overrides it (3 = German) to
         * run with the same settings as a reference console -- the xemu
         * EEPROM used for ground-truth comparisons is German, and titles
         * take language-dependent file paths (docs Update 345). */
        if (ValueLength >= sizeof(ULONG)) {
            const char *lang = getenv("RECOMP_XC_LANGUAGE");
            *(PULONG)Value = (lang && *lang) ? (ULONG)strtoul(lang, NULL, 0) : 1;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* Dashboard video settings. With the corrected high-word flags the
         * default is plain 4:3 480i -- what titles saw before, when the
         * low-bit values here were invisible to XGetVideoFlags (docs Update
         * 481). RECOMP_WIDESCREEN=1 sets the 16:9 flag (the title then
         * renders anamorphic 16:9 into its 640x480 target and the window
         * stretches it); RECOMP_XC_VIDEO=<hex> sets the raw value. */
        if (ValueLength >= sizeof(ULONG)) {
            const char *raw = getenv("RECOMP_XC_VIDEO");
            const char *ws = getenv("RECOMP_WIDESCREEN");
            ULONG flags = 0;
            if (ws && *ws && *ws != '0')
                flags |= XC_VIDEO_FLAGS_WIDESCREEN;
            if (raw && *raw)
                flags = (ULONG)strtoul(raw, NULL, 16);
            *(PULONG)Value = flags;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_AUDIO:
        /* Plain stereo, no Dolby Digital -- what xemu's generated EEPROM holds
         * (its user section leaves offset 0x98 zero). This used to be
         * 0x00010001, commented as "stereo + AC3" but actually mono + AC3:
         * DirectSound then loads its AC3 encoder onto the EP, whose code uses
         * extractu, an instruction the DSP56300 interpreter (like xemu's)
         * does not implement -- SCDA's EP hit it at P:0x0519 (docs Update 344). */
        if (ValueLength >= sizeof(ULONG)) {
            /* Diagnostic opt-in only. Keep the normal stereo EEPROM value
             * unchanged; a separate test build can query the title's
             * Surround+AC3 path without altering any game data. */
            const char *raw = getenv("RECOMP_XC_AUDIO");
            ULONG flags = XC_AUDIO_FLAGS_STEREO;
            /* The opt-in surround path (RECOMP_AUDIO_SURROUND=1, docs Update
             * 610) runs the EP on a core that executes the AC3 encoder:
             * report Surround + Dolby Digital, as a console set up that way. */
            if (mcpx_apu_surround_active()) {
                flags = XC_AUDIO_FLAGS_SURROUND | XC_AUDIO_FLAGS_ENABLE_AC3;
                fprintf(stderr, "[AUDIO-SURROUND] XC_AUDIO=0x%08X (surround path)\n",
                        flags);
            }
            if (raw && *raw) {
                char *end = NULL;
                unsigned long parsed = strtoul(raw, &end, 0);
                if (end && !*end &&
                    (parsed == XC_AUDIO_FLAGS_STEREO ||
                     parsed == XC_AUDIO_FLAGS_SURROUND ||
                     parsed == (XC_AUDIO_FLAGS_SURROUND | XC_AUDIO_FLAGS_ENABLE_AC3)))
                    flags = (ULONG)parsed;
                else
                    fprintf(stderr, "[AUDIO-SURROUND] unsupported XC_AUDIO override: %s\n", raw);
            }
            *(PULONG)Value = flags;
            if (raw && *raw)
                fprintf(stderr, "[AUDIO-SURROUND] XC_AUDIO=0x%08X\n", flags);
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_P_CONTROL_GAMES:
    case XC_P_CONTROL_MOVIES:
        /* Unrestricted. Titles compare their XBE certificate's rating against
         * this and refuse to run (Halo boots to the dashboard) if the console
         * is configured more strictly, so 0 means "no restriction". */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        /* Region 1 (North America) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_TIMEZONE_BIAS:
        /* UTC-5 (Eastern Time) in minutes: -300 */
        if (ValueLength >= sizeof(LONG)) {
            *(PLONG)Value = -300;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(LONG);
        }
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status. Xbox had a built-in 100Mbit
 * NIC. For PC, we report link-up since we'll handle networking differently.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    /* Return link up, 100 Mbps, full duplex */
    return 0x01; /* XNET_ETHERNET_LINK_ACTIVE */
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
