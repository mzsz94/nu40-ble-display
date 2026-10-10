/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NU40_KIT_IO_H_
#define NU40_KIT_IO_H_

#include <stdbool.h>
#include <stdint.h>

enum kit_action {
	KIT_BEEP = 1,
	KIT_RECORD,
	KIT_PLAY,
	KIT_SD_RETRY,
	KIT_PAGE,
	KIT_RGB,
};

struct kit_status {
	uint32_t mic_blocks, mic_errors, peak, rms;
	uint32_t recorded_samples, records, beeps, plays, sd_saves, sd_reads;
	int32_t mic_result, speaker_result, sd_result, audio_result, rgb_result;
	uint8_t keys_ready, pressed, seen, page, rgb_mode, busy;
	bool sd_mounted;
	char last_file[40];
};

void kit_init(void);
void kit_poll(void);
void kit_snapshot(struct kit_status *out);
int kit_request(enum kit_action action, uint32_t value);
void kit_set_link(bool connected);
void kit_local_screen(char out[85]);
void kit_report(void);

#endif
