/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdlib.h>
#include "wallaby_graphics.h"

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
	puts("PASS WG1 strict parsing, bounds, warning/stale priority and cosmetic blink");
	return 0;
}
