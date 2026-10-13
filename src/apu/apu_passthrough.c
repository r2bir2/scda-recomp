/*
 * Dolby Digital passthrough for the surround path (docs Update 612).
 *
 * With XC_AUDIO Surround+AC3 the EP writes an IEC 61937 stream to its
 * FIFO #1 -- the S/PDIF output of the console, AC-3 bursts in a 48 kHz
 * 16-bit stereo carrier (192 KB/s). This hands that stream unchanged to a
 * Windows output that accepts it (WASAPI exclusive mode,
 * KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL), as a movie player does: an
 * optical/coax S/PDIF output, so the receiver decodes the game's own 5.1.
 * Opt-in: RECOMP_AUDIO_SURROUND=passthrough. The producer is the EP worker
 * (apu_surround.c), the consumer this file's render thread; between them a
 * single-producer/single-consumer byte ring, whose fill level also paces the
 * APU (apu_core.c throttle) the way the XAudio2 queue does otherwise.
 */

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apu_passthrough.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

static const CLSID s_clsid_enum = { 0xBCDE0395, 0xE52F, 0x467C,
    { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID s_iid_enum = { 0xA95664D2, 0x9614, 0x4F35,
    { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID s_iid_client = { 0x1CB9AD4C, 0xDBFA, 0x4c32,
    { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID s_iid_render = { 0xF294ACFC, 0x3146, 0x4483,
    { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
/* KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL */
static const GUID s_sub_ac3 = { 0x00000092, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

#define RING_BYTES   (1u << 16)   /* 64 KiB, ~340 ms of carrier */
#define BLOCK_BYTES  1024u        /* one 256-sample block of the carrier */

static uint8_t s_ring[RING_BYTES];
static volatile LONG s_head, s_tail;   /* byte counters, wrap at 2^32 */
static volatile LONG s_running, s_stop, s_muted, s_bg_muted;
static HANDLE s_thread, s_event;
static IAudioClient *s_client;
static IAudioRenderClient *s_render;
static UINT32 s_buffer_frames;
static volatile LONG s_underruns, s_overruns;
static wchar_t s_name[128];

static unsigned ring_level(void)
{
    return (unsigned)(s_head - s_tail);
}

void apu_passthrough_feed(const uint8_t *buf, size_t len)
{
    if (!s_running) {
        return;
    }
    if (ring_level() + len > RING_BYTES) {
        /* Consumer stalled: drop this piece whole rather than tear it. */
        InterlockedIncrement(&s_overruns);
        return;
    }
    LONG head = s_head;
    for (size_t i = 0; i < len; i++) {
        s_ring[(uint32_t)(head + i) % RING_BYTES] = buf[i];
    }
    MemoryBarrier();
    InterlockedExchangeAdd(&s_head, (LONG)len);
}

void apu_passthrough_set_muted(int muted)
{
    s_bg_muted = muted;
}

int apu_passthrough_active(void)
{
    return s_running != 0;
}

int apu_passthrough_queued_blocks(void)
{
    return s_running ? (int)(ring_level() / BLOCK_BYTES) : -1;
}

static void fill(BYTE *dst, UINT32 frames)
{
    unsigned want = frames * 4u, have = ring_level();
    LONG tail = s_tail;
    if (have < want) {
        /* Not enough: send silence and keep the data for the next period, so
         * a burst is never split by a gap. */
        memset(dst, 0, want);
        if (s_head != s_tail || have) {
            InterlockedIncrement(&s_underruns);
        }
        return;
    }
    for (unsigned i = 0; i < want; i++) {
        dst[i] = s_ring[(uint32_t)(tail + i) % RING_BYTES];
    }
    if (s_muted || s_bg_muted) {
        memset(dst, 0, want);
    }
    MemoryBarrier();
    InterlockedExchangeAdd(&s_tail, (LONG)want);
}

static DWORD WINAPI render_thread(LPVOID arg)
{
    DWORD task = 0;
    HANDLE mmcss;
    DWORD last = GetTickCount();
    LONG und_prev = 0, ovr_prev = 0;
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (!s_stop) {
        BYTE *data = NULL;
        if (WaitForSingleObject(s_event, 200) != WAIT_OBJECT_0) {
            continue;
        }
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(s_render, s_buffer_frames, &data))) {
            fill(data, s_buffer_frames);
            IAudioRenderClient_ReleaseBuffer(s_render, s_buffer_frames, 0);
        }
        if (GetTickCount() - last >= 10000) {
            if (s_underruns != und_prev || s_overruns != ovr_prev) {
                fprintf(stderr, "[AC3-OUT] underruns=%ld (+%ld) overruns=%ld (+%ld) "
                        "ring=%u B\n", s_underruns, s_underruns - und_prev,
                        s_overruns, s_overruns - ovr_prev, ring_level());
            }
            und_prev = s_underruns;
            ovr_prev = s_overruns;
            last = GetTickCount();
        }
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    CoUninitialize();
    return 0;
}

/* The render endpoint: RECOMP_AUDIO_DEVICE=<part of its name> if set, else
 * the Windows default output. */
static IMMDevice *pick_device(IMMDeviceEnumerator *en)
{
    const char *want = getenv("RECOMP_AUDIO_DEVICE");
    IMMDevice *dev = NULL;
    if (want && *want) {
        IMMDeviceCollection *col = NULL;
        UINT n = 0;
        wchar_t wwant[128];
        MultiByteToWideChar(CP_ACP, 0, want, -1, wwant, 128);
        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender,
                DEVICE_STATE_ACTIVE, &col))) {
            IMMDeviceCollection_GetCount(col, &n);
            for (UINT i = 0; i < n && !dev; i++) {
                IMMDevice *d = NULL;
                IPropertyStore *ps = NULL;
                PROPVARIANT name;
                PropVariantInit(&name);
                IMMDeviceCollection_Item(col, i, &d);
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) &&
                    SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &name)) &&
                    name.vt == VT_LPWSTR && wcsstr(name.pwszVal, wwant)) {
                    dev = d;
                    d = NULL;
                }
                PropVariantClear(&name);
                if (ps) IPropertyStore_Release(ps);
                if (d) IMMDevice_Release(d);
            }
            IMMDeviceCollection_Release(col);
        }
        if (!dev) {
            fprintf(stderr, "[AC3-OUT] no output matches RECOMP_AUDIO_DEVICE=%s, "
                            "using the default\n", want);
        }
    }
    if (!dev) {
        IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    }
    if (dev) {
        IPropertyStore *ps = NULL;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) &&
            SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            wcsncpy_s(s_name, 128, name.pwszVal, _TRUNCATE);
        }
        PropVariantClear(&name);
        if (ps) IPropertyStore_Release(ps);
    }
    return dev;
}

static HRESULT init_client(IMMDevice *dev, const WAVEFORMATEX *fmt)
{
    REFERENCE_TIME period = 0, min_period = 0;
    HRESULT hr = IMMDevice_Activate(dev, &s_iid_client, CLSCTX_ALL, NULL,
                                    (void **)&s_client);
    if (FAILED(hr)) return hr;
    IAudioClient_GetDevicePeriod(s_client, &period, &min_period);
    hr = IAudioClient_Initialize(s_client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                 period, fmt, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        /* Retry with the period the device aligned the buffer to. */
        UINT32 frames = 0;
        IAudioClient_GetBufferSize(s_client, &frames);
        IAudioClient_Release(s_client);
        s_client = NULL;
        period = (REFERENCE_TIME)(10000000.0 * frames / 48000 + 0.5);
        hr = IMMDevice_Activate(dev, &s_iid_client, CLSCTX_ALL, NULL,
                                (void **)&s_client);
        if (FAILED(hr)) return hr;
        hr = IAudioClient_Initialize(s_client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                     period, fmt, NULL);
    }
    return hr;
}

int apu_passthrough_start(int muted)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEXTENSIBLE f = { 0 };
    BYTE *data = NULL;
    HRESULT hr;

    if (s_running) {
        return 1;
    }
    s_muted = muted;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&s_clsid_enum, NULL, CLSCTX_ALL, &s_iid_enum, (void **)&en);
    if (FAILED(hr) || !(dev = pick_device(en))) {
        fprintf(stderr, "[AC3-OUT] no audio output (0x%08lX)\n", (unsigned long)hr);
        goto fail;
    }

    /* IEC 61937 AC-3: the stream rides a 48 kHz 16-bit stereo carrier. */
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = 48000;
    f.Format.wBitsPerSample = 16;
    f.Format.nBlockAlign = 4;
    f.Format.nAvgBytesPerSec = 192000;
    f.Format.cbSize = sizeof(f) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 16;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = s_sub_ac3;

    hr = init_client(dev, &f.Format);
    if (FAILED(hr)) {
        fprintf(stderr, "[AC3-OUT] \"%ls\" does not take Dolby Digital in exclusive "
                "mode (0x%08lX)%s\n", s_name, (unsigned long)hr,
                hr == AUDCLNT_E_UNSUPPORTED_FORMAT ? ": not an S/PDIF/HDMI bitstream output" :
                hr == AUDCLNT_E_DEVICE_IN_USE ? ": in use by another program" :
                hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ?
                    ": exclusive mode is switched off in its Windows properties" : "");
        goto fail;
    }
    s_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(s_client, s_event);
    IAudioClient_GetBufferSize(s_client, &s_buffer_frames);
    hr = IAudioClient_GetService(s_client, &s_iid_render, (void **)&s_render);
    if (FAILED(hr)) {
        fprintf(stderr, "[AC3-OUT] GetService failed (0x%08lX)\n", (unsigned long)hr);
        goto fail;
    }
    /* Start with one period of silence. */
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(s_render, s_buffer_frames, &data))) {
        IAudioRenderClient_ReleaseBuffer(s_render, s_buffer_frames,
                                         AUDCLNT_BUFFERFLAGS_SILENT);
    }
    hr = IAudioClient_Start(s_client);
    if (FAILED(hr)) {
        fprintf(stderr, "[AC3-OUT] Start failed (0x%08lX)\n", (unsigned long)hr);
        goto fail;
    }
    s_stop = 0;
    s_running = 1;
    s_thread = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
    fprintf(stderr, "[AC3-OUT] Dolby Digital passthrough to \"%ls\" (exclusive, "
            "%u-frame periods)%s\n", s_name, s_buffer_frames,
            muted ? " -- muted (zeros)" : "");
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return 1;

fail:
    if (s_render) { IAudioRenderClient_Release(s_render); s_render = NULL; }
    if (s_client) { IAudioClient_Release(s_client); s_client = NULL; }
    if (s_event) { CloseHandle(s_event); s_event = NULL; }
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return 0;
}

/* Releases the exclusive device: a title relaunch (xa2_mute) starts a new
 * process that has to open it, and the old one lives on until the new one
 * has drawn (docs Update 502). */
void apu_passthrough_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = 0;
    s_stop = 1;
    if (s_thread) {
        WaitForSingleObject(s_thread, 1000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    IAudioClient_Stop(s_client);
    IAudioRenderClient_Release(s_render);
    s_render = NULL;
    IAudioClient_Release(s_client);
    s_client = NULL;
    CloseHandle(s_event);
    s_event = NULL;
    fprintf(stderr, "[AC3-OUT] passthrough stopped, device released\n");
}
