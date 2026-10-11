/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_GRAPHICS_H_
#define WALLABY_GRAPHICS_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WALLABY_PIXELS 512U
#define WALLABY_LABEL_COLUMNS 10U
#define WALLABY_SCENE_TIMEOUT_MS 3000U

/* ASCII carried by the existing acknowledged TEXT characteristic, not a video
 * stream. R/P/T are whole degrees from real gravity measurements. P/S describe
 * virtual patrol/stopped state; they never assert physical motor movement.
 */
struct wallaby_scene {
	char view;                 /* F: face, H: horizon */
	char state;                /* L/R/C/P/S/A/B/X: live/reference/calibrate/patrol/stop/tilt/alarm/stale */
	int16_t roll, pitch;
	uint16_t tilt;
	uint32_t sample;
};

static inline bool wallaby_scene_valid(const struct wallaby_scene *s)
{
	return s && (s->view == 'F' || s->view == 'H') &&
	       s->state && strchr("LRCPSABX", s->state) &&
	       s->roll >= -180 && s->roll <= 180 &&
	       s->pitch >= -90 && s->pitch <= 90 && s->tilt <= 180 &&
	       (s->sample != 0 || s->state == 'X' || s->state == 'B');
}

static inline bool wallaby_number(const char **cursor, bool sign, uint32_t maximum,
				 int64_t *value)
{
	const char *p = *cursor;
	bool negative = false;
	uint32_t n = 0;

	if (sign && *p == '-') {
		negative = true;
		p++;
	}
	if (*p < '0' || *p > '9') {
		return false;
	}
	for (; *p >= '0' && *p <= '9'; p++) {
		uint32_t digit = (uint32_t)(*p - '0');

		if (n > maximum / 10 || (n == maximum / 10 && digit > maximum % 10)) {
			return false;
		}
		n = n * 10 + digit;
	}
	*cursor = p;
	*value = negative ? -(int64_t)n : (int64_t)n;
	return true;
}

/* Return 0 for ordinary text, 1 for a complete scene, -1 for malformed reserved
 * commands. Never change the caller's scene on a rejected command.
 */
static inline int wallaby_scene_parse(const char *text, struct wallaby_scene *out)
{
	struct wallaby_scene s = {0};
	const char *p;
	int64_t n;

	if (!text || !out) {
		return -1;
	}
	if (strncmp(text, "@WG", 3) != 0) {
		return 0;
	}
	if (strncmp(text, "@WG1 ", 5) != 0 || strlen(text) < 15) {
		return -1;
	}
	s.view = text[5];
	s.state = text[7];
	if (text[6] != ' ' || text[8] != '\n') {
		return -1;
	}
	p = text + 9;
	if (!wallaby_number(&p, true, 180, &n) || *p++ != ' ') {
		return -1;
	}
	s.roll = (int16_t)n;
	if (!wallaby_number(&p, true, 90, &n) || *p++ != ' ') {
		return -1;
	}
	s.pitch = (int16_t)n;
	if (!wallaby_number(&p, false, 180, &n) || *p++ != '\n') {
		return -1;
	}
	s.tilt = (uint16_t)n;
	if (!wallaby_number(&p, false, UINT32_MAX, &n) || *p != '\0') {
		return -1;
	}
	s.sample = (uint32_t)n;
	if (!wallaby_scene_valid(&s)) {
		return -1;
	}
	*out = s;
	return 1;
}

static inline void wallaby_pixel(uint8_t *p, int x, int y, bool on)
{
	if (x >= 0 && x < 64 && y >= 0 && y < 32) {
		uint8_t mask = (uint8_t)(1U << (y % 8));
		uint8_t *byte = &p[(y / 8) * 128 + x];

		*byte = on ? *byte | mask : *byte & (uint8_t)~mask;
	}
}

static inline void wallaby_rect(uint8_t *p, int x, int y, int w, int h, bool fill)
{
	for (int row = 0; row < h; row++) {
		for (int col = 0; col < w; col++) {
			if (fill || row == 0 || row == h - 1 || col == 0 || col == w - 1) {
				wallaby_pixel(p, x + col, y + row, true);
			}
		}
	}
}

static inline int wallaby_abs(int n) { return n < 0 ? -n : n; }
static inline int wallaby_clip(int n, int low, int high)
{
	return n < low ? low : n > high ? high : n;
}

static inline void wallaby_line(uint8_t *p, int x, int y, int end_x, int end_y)
{
	int dx = wallaby_abs(end_x - x), dy = -wallaby_abs(end_y - y);
	int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1, error = dx + dy;

	for (;;) {
		wallaby_pixel(p, x, y, true);
		if (x == end_x && y == end_y) {
			break;
		}
		int twice = 2 * error;

		if (twice >= dy) { error += dy; x += sx; }
		if (twice <= dx) { error += dx; y += sy; }
	}
}

/* Fixed point sine at five-degree knots, linearly interpolated. */
static inline int wallaby_sin(int degrees)
{
	static const int16_t table[] = {
		0, 87, 174, 259, 342, 423, 500, 574, 643, 707,
		766, 819, 866, 906, 940, 966, 985, 996, 1000,
	};
	int sign = degrees < 0 ? -1 : 1;
	int angle = wallaby_abs(degrees) % 360;

	if (angle > 180) { sign = -sign; angle -= 180; }
	if (angle > 90) { angle = 180 - angle; }
	int value = table[angle / 5];

	if (angle < 90) {
		value += (table[angle / 5 + 1] - value) * (angle % 5) / 5;
	}
	return sign * value;
}

static inline const char *wallaby_state_label(char state)
{
	switch (state) {
	case 'R': return "SET REF";
	case 'C': return "CALIBRATE";
	case 'P': return "SIM PATROL";
	case 'S': return "SIM STOP";
	case 'A': return "TILT STOP";
	case 'B': return "SIM ALERT";
	case 'X': return "LINK STALE";
	default: return "LIVE IMU";
	}
}

/* Only the cosmetic blink runs on a local clock. Gaze/horizon use real R/P;
 * stale data replaces the animation, never supplies an invented attitude.
 */
static inline bool wallaby_scene_render(const struct wallaby_scene *s, bool expired,
				       uint32_t now, uint8_t pixels[WALLABY_PIXELS],
				       char labels[4][WALLABY_LABEL_COLUMNS + 1])
{
	if (!wallaby_scene_valid(s) || !pixels || !labels) {
		return false;
	}
	memset(pixels, 0, WALLABY_PIXELS);
	memset(labels, 0, 4 * (WALLABY_LABEL_COLUMNS + 1));
	char state = s->state == 'A' || s->state == 'B' ? s->state :
		     expired ? 'X' : s->state;

	snprintf(labels[0], WALLABY_LABEL_COLUMNS + 1, "%s", wallaby_state_label(state));
	if (state == 'X') {
		wallaby_rect(pixels, 17, 10, 30, 15, false);
		wallaby_rect(pixels, 23, 3, 4, 7, true);
		wallaby_rect(pixels, 37, 3, 4, 7, true);
		wallaby_line(pixels, 10, 30, 54, 1);
		strcpy(labels[1], "NO FRESH");
		strcpy(labels[2], "ATTITUDE");
		strcpy(labels[3], "CHECK LINK");
		return true;
	}
	if (state == 'A' || state == 'B') {
		wallaby_line(pixels, 32, 1, 3, 30);
		wallaby_line(pixels, 32, 1, 61, 30);
		wallaby_line(pixels, 3, 30, 61, 30);
		wallaby_rect(pixels, 30, 11, 5, 10, true);
		wallaby_rect(pixels, 30, 24, 5, 3, true);
	} else if (s->view == 'H') {
		int center_y = 16 + wallaby_clip(s->pitch, -45, 45) * 10 / 45;
		int dx = 26 * wallaby_sin(s->roll + 90) / 1000;
		int dy = 26 * wallaby_sin(s->roll) / 1000;

		wallaby_rect(pixels, 1, 1, 62, 30, false);
		wallaby_line(pixels, 32 - dx, center_y - dy, 32 + dx, center_y + dy);
		wallaby_line(pixels, 22, 16, 28, 16);
		wallaby_line(pixels, 36, 16, 42, 16);
		wallaby_rect(pixels, 31, 14, 3, 5, false);
	} else {
		bool blink = now % 3600U < 140U && state != 'C';
		int gaze_x = wallaby_clip(s->roll / 8, -5, 5);
		int gaze_y = wallaby_clip(-s->pitch / 12, -2, 2);

		wallaby_rect(pixels, 7, 4, 50, 26, false);
		wallaby_rect(pixels, 12, 0, 5, 4, true);
		wallaby_rect(pixels, 47, 0, 5, 4, true);
		for (int eye = 0; eye < 2; eye++) {
			int x = 14 + eye * 25;

			wallaby_rect(pixels, x, blink ? 16 : 10, 11, blink ? 2 : 12, true);
			if (!blink) {
				for (int y = 0; y < 5; y++) {
					for (int col = 0; col < 3; col++) {
						wallaby_pixel(pixels, x + 4 + col + gaze_x,
							      13 + y + gaze_y, false);
					}
				}
			}
		}
		wallaby_line(pixels, 26, 26, 38, 26);
		if (state == 'P') {
			wallaby_line(pixels, 27, 27, 37, 27);
		}
	}
	snprintf(labels[1], WALLABY_LABEL_COLUMNS + 1, "R%+d", s->roll);
	snprintf(labels[2], WALLABY_LABEL_COLUMNS + 1, "P%+d T%u", (int)(s->pitch % 91), (unsigned int)(s->tilt % 181));
	if (expired) {
		strcpy(labels[1], "NO FRESH");
		strcpy(labels[2], "ALERT HELD");
	}
	if (state == 'B') {
		strcpy(labels[1], "TRAINING");
		strcpy(labels[2], "ACK/CLEAR");
	}
	/* Last nine digits fit the physical display. Full sample is kept by host. */
	snprintf(labels[3], WALLABY_LABEL_COLUMNS + 1, "#%lu",
		 (unsigned long)(s->sample % 1000000000U));
	return true;
}

#endif /* WALLABY_GRAPHICS_H_ */
