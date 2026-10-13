/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include "apu_passthrough.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_MAX_CHANNELS  6      /* 5.1 for the surround path (Update 611) */
#define XA2_BUF_SAMPLES   256    /* eight 32-sample APU frames, ~5.33 ms */
/* Ring of submitted blocks. apu_core.c paces the APU by the queue level
 * (xa2_queued, docs Update 506), normally ~6 blocks (32 ms); the ring is
 * large enough that a queued block is never overwritten. */
#define XA2_NUM_BUFS      32

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][XA2_MAX_CHANNELS];
/* Interleaved channels per sample frame: 2, or 6 (FL FR FC LFE BL BR) when
 * the surround path selected it before xa2_init (xa2_set_channels). */
static int                     XA2_CHANNELS = 2;
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static unsigned                g_xa2_dropped = 0;
static unsigned                g_xa2_nonzero = 0;
static FILE                   *g_xa2_wav = NULL;
static uint32_t                g_xa2_wav_bytes = 0;
static uint32_t                g_xa2_wav_seconds = 30;
/* Automated runs with RECOMP_SMOKE_MUTE (docs Update 616): the card gets
 * silence, the diagnostic WAV capture still the real PCM. */
static int                     g_xa2_silent = 0;

/* Optional 30-second diagnostic capture of the exact PCM offered to XAudio2.
 * Rewrites the WAV length after each submitted block so a forced-stop run
 * still leaves a valid recording. Each process gets its own file. */
static void xa2_wav_header(void)
{
    uint32_t size = g_xa2_wav_bytes + 36;
    uint32_t fmt_size = 16;
    uint16_t pcm = 1, channels = XA2_CHANNELS, align = XA2_CHANNELS * 2;
    uint16_t bits = 16;
    uint32_t rate = XA2_SAMPLE_RATE, bytes_per_sec = XA2_SAMPLE_RATE * align;
    if (!g_xa2_wav) return;
    fseek(g_xa2_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_xa2_wav); fwrite(&size, 4, 1, g_xa2_wav);
    fwrite("WAVEfmt ", 1, 8, g_xa2_wav);
    fwrite(&fmt_size, 4, 1, g_xa2_wav);
    fwrite(&pcm, 2, 1, g_xa2_wav); fwrite(&channels, 2, 1, g_xa2_wav);
    fwrite(&rate, 4, 1, g_xa2_wav);
    fwrite(&bytes_per_sec, 4, 1, g_xa2_wav);
    fwrite(&align, 2, 1, g_xa2_wav); fwrite(&bits, 2, 1, g_xa2_wav);
    fwrite("data", 1, 4, g_xa2_wav);
    fwrite(&g_xa2_wav_bytes, 4, 1, g_xa2_wav);
    fseek(g_xa2_wav, 0, SEEK_END);
}

void xa2_set_silent(int silent)
{
    g_xa2_silent = silent;
}

void xa2_set_channels(int channels)
{
    if (!g_xa2_initialized && (channels == 2 || channels == 6))
        XA2_CHANNELS = channels;
}

int xa2_init(void)
{
    HRESULT hr;
    WAVEFORMATEXTENSIBLE wfx = { 0 };

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != (HRESULT)0x80010106 /* RPC_E_CHANGED_MODE */ && hr != S_FALSE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        return 0;
    }

    /* 5.1 goes to the device's own layout (XAudio2 downmixes it for a
     * stereo device); stereo keeps the fixed stereo master as before. */
    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS == 2 ? XA2_CHANNELS : XAUDIO2_DEFAULT_CHANNELS,
        XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    wfx.Format.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.Format.nChannels       = (WORD)XA2_CHANNELS;
    wfx.Format.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.Format.wBitsPerSample  = 16;
    wfx.Format.nBlockAlign     = (WORD)(XA2_CHANNELS * 2);
    wfx.Format.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.Format.nBlockAlign;
    if (XA2_CHANNELS == 6) {
        static const GUID pcm_guid = { 0x00000001, 0x0000, 0x0010,
            { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.cbSize = sizeof(wfx) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = 16;
        wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                            SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                            SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        wfx.SubFormat = pcm_guid;
    }

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] SourceVoice_Start failed: 0x%08lX\n", hr);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        IXAudio2_Release(g_xa2);
        g_xa2_source = NULL;
        g_xa2_master = NULL;
        g_xa2 = NULL;
        return 0;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_dropped = 0;
    g_xa2_nonzero = 0;
    if (getenv("RECOMP_AUDIO_WAV")) {
        char path[80];
        snprintf(path, sizeof(path), "scda_audio_xa2_%lu.wav",
                 (unsigned long)GetCurrentProcessId());
        g_xa2_wav = fopen(path, "wb");
        g_xa2_wav_bytes = 0;
        xa2_wav_header();
        fprintf(stderr, "[XA2] diagnostic PCM capture: %s (%s)\n", path,
                g_xa2_wav ? "opened" : "open failed");
    }

    {
        XAUDIO2_VOICE_DETAILS md;
        g_xa2_master->lpVtbl->GetVoiceDetails(g_xa2_master, &md);
        fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz %s 16-bit, %d x %d-sample "
                "buffers, device mix %u channels)\n", XA2_SAMPLE_RATE,
                XA2_CHANNELS == 6 ? "5.1" : "stereo", XA2_NUM_BUFS,
                XA2_BUF_SAMPLES, md.InputChannels);
    }
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_xa2_initialized) return;

    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    if (g_xa2_wav) {
        xa2_wav_header();
        fclose(g_xa2_wav);
        g_xa2_wav = NULL;
    }
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Blocks submitted and not yet played (-1 when inactive). */
int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source)
        return -1;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

/* Silence the output for good (a title relaunch: the old instance lives on
 * until the new one has drawn, docs Update 502). */
void xa2_mute(void)
{
    /* The passthrough output holds the device exclusively; the relaunched
     * process has to open it (docs Update 612). */
    apu_passthrough_stop();
    if (g_xa2_initialized && g_xa2_source)
        IXAudio2SourceVoice_SetVolume(g_xa2_source, 0.0f, XAUDIO2_COMMIT_NOW);
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;
    static DWORD last_report;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        g_xa2_dropped++;
        return 0;
    }
    {
        /* Audio diagnosis (docs Update 507): an empty queue here means the
         * card already ran dry (an audible gap); reported every 5 s when it
         * changed. F7 (game window focused) records the next 30 s of exactly
         * this PCM to scda_audio_f7_<pid>_<n>.wav next to the exe. */
        static unsigned underruns, reported_underruns, reported_dropped, f7_no;
        static DWORD last_check, last_line;
        static int f7_was_down;
        DWORD now = GetTickCount();
        if (state.BuffersQueued == 0 && g_xa2_frames_written > 200)
            underruns++;
        if (now - last_line >= 5000) {
            if (underruns != reported_underruns || g_xa2_dropped != reported_dropped)
                fprintf(stderr, "[XA2] underruns=%u (+%u) dropped=%u (+%u) in 5 s\n",
                        underruns, underruns - reported_underruns, g_xa2_dropped,
                        g_xa2_dropped - reported_dropped);
            reported_underruns = underruns;
            reported_dropped = g_xa2_dropped;
            last_line = now;
        }
        if (now - last_check >= 50) {
            DWORD pid = 0;
            HWND fg = GetForegroundWindow();
            int down;
            if (fg)
                GetWindowThreadProcessId(fg, &pid);
            down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F7) & 0x8000);
            if (down && !f7_was_down && !g_xa2_wav) {
                char path[96];
                snprintf(path, sizeof(path), "scda_audio_f7_%lu_%u.wav",
                         (unsigned long)GetCurrentProcessId(), ++f7_no);
                g_xa2_wav = fopen(path, "wb");
                g_xa2_wav_bytes = 0;
                if (g_xa2_wav)
                    xa2_wav_header();
                fprintf(stderr, "[XA2] F7: recording 30 s to %s (%s), underruns so "
                        "far %u, at tick %lu ms\n", path,
                        g_xa2_wav ? "ok" : "open failed", underruns,
                        (unsigned long)now);
            }
            f7_was_down = down;
            last_check = now;
        }
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));
    if (g_xa2_silent)
        memset(g_xa2_bufs[idx], 0, copy_samples * XA2_CHANNELS * sizeof(int16_t));
    /* RECOMP_AUDIO_WAV_AT=<s>: capture RECOMP_AUDIO_WAV_SECONDS (default 30)
     * of this PCM starting <s> seconds of audio into the run, e.g. once a
     * save has loaded (docs Update 616). */
    {
        static int at = -2;
        if (at == -2) {
            const char *e = getenv("RECOMP_AUDIO_WAV_AT");
            const char *d = getenv("RECOMP_AUDIO_WAV_SECONDS");
            at = e ? atoi(e) : -1;
            if (d && atoi(d) > 0 && atoi(d) <= 600)
                g_xa2_wav_seconds = (uint32_t)atoi(d);
        }
        if (at >= 0 && !g_xa2_wav &&
            (double)g_xa2_frames_written * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE >= at) {
            char path[96];
            snprintf(path, sizeof(path), "scda_audio_at%d_%lu.wav", at,
                     (unsigned long)GetCurrentProcessId());
            g_xa2_wav = fopen(path, "wb");
            g_xa2_wav_bytes = 0;
            if (g_xa2_wav)
                xa2_wav_header();
            fprintf(stderr, "[XA2] recording %u s to %s\n", g_xa2_wav_seconds, path);
            at = -1;
        }
    }

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] SubmitSourceBuffer failed: 0x%08lX\n", hr);
        return 0;
    }

    for (int i = 0; i < copy_samples * XA2_CHANNELS; i++)
        if (samples[i]) { g_xa2_nonzero++; break; }
    if (g_xa2_wav && g_xa2_wav_bytes < XA2_SAMPLE_RATE * XA2_CHANNELS * 2 * g_xa2_wav_seconds) {
        fwrite(samples, XA2_CHANNELS * sizeof(int16_t), copy_samples, g_xa2_wav);
        g_xa2_wav_bytes += copy_samples * XA2_CHANNELS * sizeof(int16_t);
        if (!(g_xa2_frames_written % 188)) {
            xa2_wav_header();
            fflush(g_xa2_wav);
        }
        if (g_xa2_wav_bytes >= XA2_SAMPLE_RATE * XA2_CHANNELS * 2 * g_xa2_wav_seconds) {
            xa2_wav_header();
            fclose(g_xa2_wav);
            g_xa2_wav = NULL;
        }
    }
    if (getenv("RECOMP_AUDIO_DIAG") && GetTickCount() - last_report >= 1000) {
        fprintf(stderr, "[XA2] blocks=%d nonzero_blocks=%u dropped=%u queued=%u "
                "pcm_seconds=%.2f\n", g_xa2_frames_written, g_xa2_nonzero,
                g_xa2_dropped, state.BuffersQueued,
                (double)g_xa2_frames_written * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE);
        last_report = GetTickCount();
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

void xa2_set_channels(int c)                          { (void)c; }
void xa2_set_silent(int s)                            { (void)s; }
int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_queued(void)                                 { return -1; }
void xa2_mute(void)                                   {}
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }

#endif /* _WIN32 */
