/*
 * Dolby Digital passthrough of the EP's IEC 61937 stream to an S/PDIF or
 * HDMI output (WASAPI exclusive), docs Update 612. See apu_passthrough.c.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Opens the output (RECOMP_AUDIO_DEVICE name part, else the default) in
 * exclusive Dolby Digital mode; 1 on success. `muted` sends zeros. */
int apu_passthrough_start(int muted);
void apu_passthrough_stop(void);
int apu_passthrough_active(void);
/* Zeros instead of the stream while set (background mute, Update 614). */
void apu_passthrough_set_muted(int muted);
/* EP FIFO #1 bytes (EP worker thread). */
void apu_passthrough_feed(const uint8_t *buf, size_t len);
/* Queued 256-sample blocks of the carrier, -1 when inactive (APU pacing). */
int apu_passthrough_queued_blocks(void);
