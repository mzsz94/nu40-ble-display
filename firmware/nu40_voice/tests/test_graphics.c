/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdlib.h>
#include "display_policy.h"

static void text_voice_composition(void)
{
	char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];
	const char *text = "LIVE IMU\nR+12 P-8\nT15 #42";
	const enum voice_status states[] = {VOICE_ARMED, VOICE_RECORDING, VOICE_READY,
					   VOICE_SENDING, VOICE_SENT, VOICE_ERROR};
	const char *labels[] = {"MIC READY", "MIC REC", "BTN3 SEND", "MIC SEND", "MIC SENT", "MIC ERROR"};

	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		assert(wallaby_display_text(text, states[i], 0, lines));
		assert(strcmp(lines[0], "LIVE IMU") == 0);
		assert(strcmp(lines[1], "R+12 P-8") == 0);
		assert(strcmp(lines[2], "T15 #42") == 0);
		assert(strcmp(lines[3], labels[i]) == 0);
	}
	assert(wallaby_display_text(text, VOICE_OFF, 0, lines) && lines[3][0] == '\0');
	assert(wallaby_display_text(text, VOICE_SENT, 3000, lines) && lines[3][0] == '\0');
	assert(wallaby_display_text(text, VOICE_ERROR, 8000, lines) && lines[3][0] == '\0');
	assert(wallaby_display_text("REPORT\nSAVED\n3 ITEMS\nUSER FOURTH ROW", VOICE_RECORDING, 0, lines));
	assert(strcmp(lines[3], "USER FOURTH ROW") == 0);
	/* Automatic 21-column wrapping also owns the last payload row. */
	assert(wallaby_display_text("123456789012345678901123456789012345678901123456789012345678901LAST", VOICE_SENDING, 0, lines));
	assert(strcmp(lines[3], "LAST") == 0);
	assert(!wallaby_display_text(NULL, VOICE_RECORDING, 0, lines));
	assert(!wallaby_display_text("1\n2\n3\n4\n5", VOICE_RECORDING, 0, lines));
}

static void voice_composition(void)
{
	struct wallaby_scene scene = {.view = 'F', .state = 'L', .roll = 12, .pitch = -8,
				      .tilt = 15, .sample = 42};
	uint8_t pixels[WALLABY_PIXELS], baseline[WALLABY_PIXELS];
	char labels[4][WALLABY_LABEL_COLUMNS + 1], original[4][WALLABY_LABEL_COLUMNS + 1];
	const enum voice_status states[] = {VOICE_ARMED, VOICE_RECORDING, VOICE_READY,
					   VOICE_SENDING, VOICE_SENT, VOICE_ERROR};
	const char *expected[] = {"MIC READY", "MIC REC", "BTN3 SEND", "MIC SEND", "MIC SENT", "MIC ERROR"};

	for (const char *view = "FH"; *view; view++) {
		scene.view = *view;
		assert(wallaby_scene_render(&scene, false, 500, baseline, original));
		for (size_t i = 0; i < sizeof(states)/sizeof(states[0]); i++) {
			assert(wallaby_display_render(&scene, false, 500, false, states[i], 0, pixels, labels));
			assert(memcmp(pixels, baseline, sizeof(pixels)) == 0);
			assert(memcmp(labels, original, 3 * sizeof(labels[0])) == 0);
			assert(strcmp(labels[3], expected[i]) == 0);
		}
	}
	assert(wallaby_display_render(&scene, false, 500, false, VOICE_OFF, 0, pixels, labels));
	assert(memcmp(labels, original, sizeof(labels)) == 0);
	assert(wallaby_display_render(&scene, false, 500, false, VOICE_SENT, 3000, pixels, labels));
	assert(memcmp(labels, original, sizeof(labels)) == 0);
	assert(wallaby_display_render(&scene, false, 500, false, VOICE_ERROR, 8000, pixels, labels));
	assert(memcmp(labels, original, sizeof(labels)) == 0);
	assert(strcmp(wallaby_voice_label(VOICE_SENT, 2999), "MIC SENT") == 0);
	assert(strcmp(wallaby_voice_label(VOICE_ERROR, 7999), "MIC ERROR") == 0);

	/* No voice state replaces any pixel or label of warning/stale scenes. */
	for (const char *warning = "XAB"; *warning; warning++) {
		scene.state = *warning;
		for (unsigned expired = 0; expired < 2; expired++) {
			assert(wallaby_scene_render(&scene, expired, 500, baseline, original));
			assert(wallaby_display_render(&scene, expired, 500, false, VOICE_RECORDING, 0, pixels, labels));
			assert(memcmp(pixels, baseline, sizeof(pixels)) == 0);
			assert(memcmp(labels, original, sizeof(labels)) == 0);
		}
		assert(wallaby_display_uses_scene(true, &scene, false, true, true));
		/* A newer plain text command (including switching to text mode)
		 * supersedes an older remote warning, without silencing BTN4. */
		assert(!wallaby_display_uses_scene(true, &scene, false, true, false));
		assert(wallaby_display_uses_scene(true, &scene, true, true, false));
	}
	scene.state = 'L';
	struct wallaby_scene saved = scene;
	assert(wallaby_display_render(&scene, false, 500, true, VOICE_SENDING, 0, pixels, labels));
	assert(strcmp(labels[0], "SIM ALERT") == 0 && strcmp(labels[1], "TRAINING") == 0);
	assert(strcmp(labels[3], "#42") == 0);
	assert(memcmp(&scene, &saved, sizeof(scene)) == 0);
	assert(wallaby_display_uses_scene(false, &scene, true, true, false));
	assert(wallaby_display_render(&scene, false, 500, false, VOICE_OFF, 0, pixels, labels));
	assert(strcmp(labels[0], "LIVE IMU") == 0);

	/* Host deliberately pauses WG1 while an answer is readable. The cached
	 * sample ages out after 3s, but cannot prematurely erase this 8s hold. */
	assert(wallaby_text_holding(true, 1000, 8999));
	assert(!wallaby_display_uses_scene(true, &scene, false, wallaby_text_holding(true, 1000, 5000), false));
	assert(!wallaby_display_uses_scene(true, &scene, false, wallaby_text_holding(true, 1000, 8999), false));
	assert(wallaby_display_uses_scene(true, &scene, false, wallaby_text_holding(true, 1000, 9000), false));
	assert(wallaby_display_render(&scene, true, 9000, false, VOICE_RECORDING, 0, pixels, labels));
	assert(strcmp(labels[0], "LINK STALE") == 0 && strcmp(labels[3], "CHECK LINK") == 0);
	assert(!wallaby_text_holding(false, 1000, 1000));
	assert(wallaby_text_holding(true, UINT32_MAX - 3999U, 3999U));
	assert(!wallaby_text_holding(true, UINT32_MAX - 3999U, 4000U));
	/* Fresh measurements received during a hold can be used at expiry. */
	scene.sample = 43; scene.roll = -30;
	assert(!wallaby_display_uses_scene(true, &scene, false, true, true));
	assert(wallaby_display_render(&scene, false, 9000, false, VOICE_OFF, 0, pixels, labels));
	assert(strcmp(labels[1], "R-30") == 0 && strcmp(labels[3], "#43") == 0);
}

static void render_bounds(void)
{
	struct {
		uint8_t before[16], pixels[WALLABY_PIXELS], after[16];
	} guarded;
	char labels[4][WALLABY_LABEL_COLUMNS + 1];
	struct wallaby_scene s = {.view = 'H', .state = 'L', .sample = UINT32_MAX};

	memset(&guarded, 0xa5, sizeof(guarded));
	for (int roll = -180; roll <= 180; roll++) {
		for (int pitch = -90; pitch <= 90; pitch += 15) {
			s.roll = roll;
			s.pitch = pitch;
			s.tilt = 180;
			assert(wallaby_scene_render(&s, false, 500, guarded.pixels, labels));
			for (int i = 0; i < 16; i++) {
				assert(guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5);
			}
			for (int i = 0; i < 4; i++) {
				assert(strlen(labels[i]) <= WALLABY_LABEL_COLUMNS);
			}
			assert(strstr(labels[2], "180"));
		}
	}
	s.view = 'F';
	for (const char *p = "LRCPSABX"; *p; p++) {
		s.state = *p;
		assert(wallaby_scene_render(&s, false, 500, guarded.pixels, labels));
		assert(strcmp(labels[0], wallaby_state_label(*p)) == 0);
	}
	s.state = 'L';
	assert(wallaby_scene_render(&s, true, 500, guarded.pixels, labels));
	assert(strcmp(labels[0], "LINK STALE") == 0);
	assert(strcmp(labels[1], "NO FRESH") == 0);
	s.state = 'A';
	assert(wallaby_scene_render(&s, true, 500, guarded.pixels, labels));
	assert(strcmp(labels[0], "TILT STOP") == 0);
	assert(strcmp(labels[2], "ALERT HELD") == 0);
	uint8_t open[WALLABY_PIXELS], closed[WALLABY_PIXELS];

	s.state = 'L';
	assert(wallaby_scene_render(&s, false, 500, open, labels));
	assert(wallaby_scene_render(&s, false, 0, closed, labels));
	assert(memcmp(open, closed, sizeof(open)) != 0);
	/* Blank right-hand pixels are reserved for the CFB 6x8 font. */
	for (int i = 0; i < 4; i++) {
		for (int x = 64; x < 128; x++) {
			assert(open[i * 128 + x] == 0);
		}
	}
}

int main(void)
{
	struct wallaby_scene s = {0}, saved;

	assert(wallaby_scene_parse("@WG1 H L\n-180 -90 180\n4294967295", &s) == 1);
	assert(s.roll == -180 && s.pitch == -90 && s.tilt == 180 && s.sample == UINT32_MAX);
	saved = s;
	const char *bad[] = {
		"@WG", "@WG2 F L\n0 0 0\n1", "@WG1 Q L\n0 0 0\n1",
		"@WG1 F Z\n0 0 0\n1", "@WG1 F L\n181 0 0\n1",
		"@WG1 F L\n0 -91 0\n1", "@WG1 F L\n0 0 181\n1",
		"@WG1 F L\n0 0 -1\n1", "@WG1 F L\n0 0 0\n0",
		"@WG1 F L\n0 0 0\n4294967296", "@WG1 F L\n0 0 0\n1x",
		"@WG1 F L\n0 0 0\n1\n", "@WG1 F L\n+0 0 0\n1",
		"@WG1 F L\n0  0 0\n1", "@WG1 F L\n0 0 0\n",
		"@WG1 F L\n-999999999999999999999 0 0\n1",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		assert(wallaby_scene_parse(bad[i], &s) == -1);
		assert(memcmp(&s, &saved, sizeof(s)) == 0);
	}
	assert(wallaby_scene_parse("VOICE RESULT\nREPORT SAVED", &s) == 0);
	assert(wallaby_scene_parse("@WG1 F X\n0 0 0\n0", &s) == 1);
	/* Every truncation and single-byte corruption of a valid command is bounded. */
	char command[] = "@WG1 F L\n-180 -90 180\n4294967295";
	for (size_t i = 0; i < sizeof(command); i++) {
		char original = command[i];

		command[i] = '\0';
		(void)wallaby_scene_parse(command, &s);
		command[i] = original;
		for (int byte = 1; byte <= 127; byte++) {
			command[i] = (char)byte;
			if (i < sizeof(command) - 1) {
				(void)wallaby_scene_parse(command, &s);
			}
		}
		command[i] = original;
	}
	render_bounds();
	voice_composition();
	text_voice_composition();
	puts("PASS WG1 strict parsing, bounds, warning/stale priority and cosmetic blink");
	return 0;
}
