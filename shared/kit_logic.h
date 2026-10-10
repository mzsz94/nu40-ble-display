/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NU40_KIT_LOGIC_H_
#define NU40_KIT_LOGIC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KIT_SAMPLE_RATE 16000U
#define KIT_RECORD_SAMPLES (KIT_SAMPLE_RATE * 2U)
#define KIT_WAV_HEADER_BYTES 44U
#define KIT_BUTTON_DEBOUNCE_MS 25U
#define KIT_BUTTON_HOLD_MS 800U

struct kit_buttons {
	uint8_t candidate, pressed, held;
	uint32_t changed[4], down_at[4];
};

struct kit_button_events {
	uint8_t rising, short_release, long_hold;
};

/* Each key has its own debounce clock; a bouncing neighbour cannot mask it. */
static inline struct kit_button_events kit_buttons_update(struct kit_buttons *s,
							 uint8_t raw, uint32_t now)
{
	struct kit_button_events e = {0};

	for (unsigned int i = 0; i < 4; i++) {
		uint8_t bit = (uint8_t)(1U << i);

		if ((raw ^ s->candidate) & bit) {
			s->candidate ^= bit;
			s->changed[i] = now;
		}
		if (((s->candidate ^ s->pressed) & bit) &&
		    (uint32_t)(now - s->changed[i]) >= KIT_BUTTON_DEBOUNCE_MS) {
			s->pressed ^= bit;
			if (s->pressed & bit) {
				s->down_at[i] = now;
				s->held &= (uint8_t)~bit;
				e.rising |= bit;
			} else if (!(s->held & bit)) {
				e.short_release |= bit;
			}
		}
		if ((s->pressed & bit) && !(s->held & bit) &&
		    (uint32_t)(now - s->down_at[i]) >= KIT_BUTTON_HOLD_MS) {
			s->held |= bit;
			e.long_hold |= bit;
		}
	}
	return e;
}

static inline uint32_t kit_isqrt(uint64_t value)
{
	uint64_t result = 0, bit = (uint64_t)1 << 62;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)result;
}

static inline uint32_t kit_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline void kit_put_le32(uint8_t *p, uint32_t value)
{
	for (unsigned int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(value >> (i * 8));
	}
}

static inline bool kit_wav_header(uint8_t *p, uint32_t samples)
{
	if (!p || !samples || samples > KIT_RECORD_SAMPLES) {
		return false;
	}
	memset(p, 0, KIT_WAV_HEADER_BYTES);
	memcpy(p, "RIFF", 4);
	kit_put_le32(p + 4, 36 + samples * 2);
	memcpy(p + 8, "WAVEfmt ", 8);
	kit_put_le32(p + 16, 16);
	p[20] = 1; /* PCM. */
	p[22] = 1; /* Mono. */
	kit_put_le32(p + 24, KIT_SAMPLE_RATE);
	kit_put_le32(p + 28, KIT_SAMPLE_RATE * 2);
	p[32] = 2;
	p[34] = 16;
	memcpy(p + 36, "data", 4);
	kit_put_le32(p + 40, samples * 2);
	return true;
}

/* Only our bounded, canonical PCM format is accepted for recording readback. */
static inline bool kit_wav_valid(const uint8_t *p, size_t length, uint32_t samples)
{
	uint8_t expected[KIT_WAV_HEADER_BYTES];

	return p && length == KIT_WAV_HEADER_BYTES && kit_wav_header(expected, samples) &&
	       memcmp(expected, p, sizeof(expected)) == 0;
}

#endif
