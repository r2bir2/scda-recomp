/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Interleaved output channels, 2 (default) or 6 (5.1, FL FR FC LFE BL BR);
 * only before xa2_init. xa2_submit_samples then takes that many per frame. */
void xa2_set_channels(int channels);

/* Silence at the card while the diagnostic WAV capture keeps the real PCM
 * (RECOMP_SMOKE_MUTE). */
void xa2_set_silent(int silent);

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Silence the output for the rest of the process (title relaunch). */
void xa2_mute(void);

/* Blocks queued in XAudio2 and not yet played; -1 when inactive. */
int xa2_queued(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

#endif /* APU_XAUDIO2_H */
