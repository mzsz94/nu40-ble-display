/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../kit_logic.h"

static void buttons(void)
{
	struct kit_buttons s = {0};
	struct kit_button_events e;

	assert(!kit_buttons_update(&s, 1, 10).rising);
	assert(!kit_buttons_update(&s, 0, 20).rising);
	assert(!kit_buttons_update(&s, 1, 30).rising);
	/* A bouncing second key cannot restart the first key's debounce. */
	assert(!kit_buttons_update(&s, 3, 40).rising);
	e = kit_buttons_update(&s, 1, 55);
	assert(e.rising == 1 && s.pressed == 1);
	assert(!kit_buttons_update(&s, 0, 70).short_release);
	e = kit_buttons_update(&s, 0, 95);
	assert(e.short_release == 1 && s.pressed == 0);

	(void)kit_buttons_update(&s, 8, 100);
	assert(kit_buttons_update(&s, 8, 125).rising == 8);
	assert(!kit_buttons_update(&s, 8, 924).long_hold);
	assert(kit_buttons_update(&s, 8, 925).long_hold == 8);
	assert(!kit_buttons_update(&s, 8, 1000).long_hold);
	(void)kit_buttons_update(&s, 0, 1100);
	assert(!kit_buttons_update(&s, 0, 1125).short_release);

	/* Both debounce and hold survive the 32-bit uptime wrap. */
	memset(&s, 0, sizeof(s));
	(void)kit_buttons_update(&s, 15, UINT32_MAX - 10);
	assert(kit_buttons_update(&s, 15, 14).rising == 15);
	assert(kit_buttons_update(&s, 15, 814).long_hold == 15);
}

static void wav(void)
{
	uint8_t header[KIT_WAV_HEADER_BYTES], copy[KIT_WAV_HEADER_BYTES];

	assert(kit_wav_header(header, KIT_RECORD_SAMPLES));
	assert(kit_get_le32(header + 4) == 64036);
	assert(kit_get_le32(header + 24) == 16000);
	assert(kit_get_le32(header + 28) == 32000);
	assert(kit_get_le32(header + 40) == 64000);
	assert(kit_wav_valid(header, sizeof(header), KIT_RECORD_SAMPLES));
	/* Every header byte is significant, including length, encoding and channels. */
	for (size_t i = 0; i < sizeof(header); i++) {
		memcpy(copy, header, sizeof(copy));
		copy[i] ^= 1;
		assert(!kit_wav_valid(copy, sizeof(copy), KIT_RECORD_SAMPLES));
	}
	assert(!kit_wav_valid(header, sizeof(header) - 1, KIT_RECORD_SAMPLES));
	assert(!kit_wav_valid(header, sizeof(header), KIT_RECORD_SAMPLES - 1));
	assert(!kit_wav_valid(NULL, 44, KIT_RECORD_SAMPLES));
	memcpy(copy, header, sizeof(copy));
	assert(!kit_wav_header(header, 0));
	assert(!kit_wav_header(header, KIT_RECORD_SAMPLES + 1));
	assert(!kit_wav_header(NULL, 1));
	assert(memcmp(header, copy, sizeof(header)) == 0);
}

static void levels(void)
{
	assert(kit_isqrt(0) == 0);
	assert(kit_isqrt(1) == 1);
	assert(kit_isqrt(15) == 3);
	assert(kit_isqrt(16) == 4);
	assert(kit_isqrt((uint64_t)32768 * 32768) == 32768);
	assert(kit_isqrt(UINT64_MAX) == UINT32_MAX);
}

int main(void)
{
	buttons();
	wav();
	levels();
	puts("PASS debounce, holds, uptime wrap, WAV bounds/corruption, PCM levels");
	return 0;
}
