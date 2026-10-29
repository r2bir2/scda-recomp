/*
 * Dolby Digital passthrough on hosts without WASAPI (ARM port, docs Update
 * 677): never active, so the EP stream is not routed anywhere.
 */
#include "apu_passthrough.h"

int apu_passthrough_start(int muted) { (void)muted; return 0; }
void apu_passthrough_stop(void) {}
int apu_passthrough_active(void) { return 0; }
void apu_passthrough_set_muted(int muted) { (void)muted; }
void apu_passthrough_feed(const uint8_t *buf, size_t len) { (void)buf; (void)len; }
int apu_passthrough_queued_blocks(void) { return -1; }
int apu_output_device_id(wchar_t *out, size_t out_len)
{
    (void)out; (void)out_len;
    return 0;
}
