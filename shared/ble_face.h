/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLE_FACE_H_
#define BLE_FACE_H_

#include "ble_display_protocol.h"

/* One complete face snapshot fits one default-MTU GATT write. */
#define BLE_DISPLAY_FACE 3U
#define BLE_FACE_FRAME_SIZE 10U
#define BLE_FACE_WIDTH 128U
#define BLE_FACE_HEIGHT 32U
#define BLE_FACE_BUFFER_SIZE 512U
#define BLE_FACE_EXPRESSIONS 5U

enum ble_face_expression {
	BLE_FACE_CURIOUS,
	BLE_FACE_HAPPY,
	BLE_FACE_SURPRISED,
	BLE_FACE_SLEEPY,
	BLE_FACE_LOVE,
};

struct ble_face {
	uint8_t expression;
	int8_t gaze_x;
	int8_t gaze_y;
	uint8_t openness;
	uint8_t mouth;
};

static inline bool ble_face_valid(const struct ble_face *face)
{
	return face != NULL && face->expression < BLE_FACE_EXPRESSIONS &&
	       face->gaze_x >= -8 && face->gaze_x <= 8 &&
	       face->gaze_y >= -4 && face->gaze_y <= 4 &&
	       face->openness <= 16 && face->mouth <= 8;
}

static inline size_t ble_face_build(const struct ble_face *face, uint16_t sequence,
				    uint8_t out[BLE_FACE_FRAME_SIZE])
{
	if (out == NULL || !ble_face_valid(face)) {
		return 0;
	}
	out[0] = BLE_DISPLAY_VERSION;
	out[1] = BLE_DISPLAY_FACE;
	out[2] = sequence & 0xff;
	out[3] = sequence >> 8;
	out[4] = face->expression;
	out[5] = face->gaze_x + 8;
	out[6] = face->gaze_y + 4;
	out[7] = face->openness;
	out[8] = face->mouth;
	out[9] = 0;
	return BLE_FACE_FRAME_SIZE;
}

/* Preserve outputs when malformed frames are rejected. */
static inline bool ble_face_parse(const uint8_t *frame, size_t length, uint16_t *sequence,
				  struct ble_face *face)
{
	struct ble_face parsed;

	if (frame == NULL || sequence == NULL || face == NULL || length != BLE_FACE_FRAME_SIZE ||
	    frame[0] != BLE_DISPLAY_VERSION || frame[1] != BLE_DISPLAY_FACE || frame[9] != 0 ||
	    frame[5] > 16 || frame[6] > 8) {
		return false;
	}
	parsed.expression = frame[4];
	parsed.gaze_x = (int)frame[5] - 8;
	parsed.gaze_y = (int)frame[6] - 4;
	parsed.openness = frame[7];
	parsed.mouth = frame[8];
	if (!ble_face_valid(&parsed)) {
		return false;
	}
	*face = parsed;
	*sequence = ble_display_u16(frame + 2);
	return true;
}

/* A clipped, page-packed monochrome buffer: bit 1 is a lit pixel. */
static inline void ble_face_pixel(uint8_t *buffer, int x, int y, bool lit)
{
	if (x >= 0 && x < (int)BLE_FACE_WIDTH && y >= 0 && y < (int)BLE_FACE_HEIGHT) {
		uint8_t *byte = &buffer[(y / 8) * BLE_FACE_WIDTH + x];
		uint8_t mask = (uint8_t)(1U << (y % 8));

		if (lit) {
			*byte |= mask;
		} else {
			*byte &= (uint8_t)~mask;
		}
	}
}

static inline void ble_face_rect(uint8_t *buffer, int x, int y, int width, int height)
{
	for (int row = 0; row < height; row++) {
		for (int column = 0; column < width; column++) {
			ble_face_pixel(buffer, x + column, y + row, true);
		}
	}
}

static inline void ble_face_ellipse(uint8_t *buffer, int x, int y, int rx, int ry, bool lit)
{
	for (int dy = -ry; dy <= ry; dy++) {
		for (int dx = -rx; dx <= rx; dx++) {
			if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry) {
				ble_face_pixel(buffer, x + dx, y + dy, lit);
			}
		}
	}
}

static inline void ble_face_heart(uint8_t *buffer, int x, int y)
{
	ble_face_ellipse(buffer, x - 4, y - 3, 5, 5, true);
	ble_face_ellipse(buffer, x + 4, y - 3, 5, 5, true);
	for (int row = 0; row < 10; row++) {
		ble_face_rect(buffer, x - 9 + row, y + row, 19 - 2 * row, 1);
	}
}

static inline bool ble_face_render(const struct ble_face *face,
				   uint8_t buffer[BLE_FACE_BUFFER_SIZE])
{
	if (buffer == NULL || !ble_face_valid(face)) {
		return false;
	}
	memset(buffer, 0, BLE_FACE_BUFFER_SIZE);
	for (int eye = 0; eye < 2; eye++) {
		int x = 42 + eye * 44 + face->gaze_x / 2;
		int y = 12 + face->gaze_y / 2;

		if (face->openness <= 2) {
			ble_face_rect(buffer, x - 11, y, 23, 2);
		} else if (face->expression == BLE_FACE_LOVE) {
			ble_face_heart(buffer, x, y);
		} else if (face->expression == BLE_FACE_HAPPY) {
			for (int dx = -10; dx <= 10; dx++) {
				ble_face_rect(buffer, x + dx, y - 5 + dx * dx / 20, 1, 3);
			}
		} else {
			int height = face->expression == BLE_FACE_SLEEPY ? 4 : face->openness;
			int top = y - height / 2;

			ble_face_rect(buffer, x - 9, top, 19, height + 1);
			ble_face_rect(buffer, x - 11, top + 2, 23, height > 3 ? height - 3 : 1);
			if (height >= 8) {
				ble_face_ellipse(buffer, x + face->gaze_x / 2,
						 y + face->gaze_y / 2, 4, height / 3, false);
			}
		}
	}
	if (face->mouth != 0) {
		ble_face_ellipse(buffer, 64, 27, 4, 1 + face->mouth / 3, true);
	} else if (face->expression == BLE_FACE_HAPPY || face->expression == BLE_FACE_LOVE) {
		for (int dx = -8; dx <= 8; dx++) {
			ble_face_pixel(buffer, 64 + dx, 26 + (64 - dx * dx) / 24, true);
		}
	} else {
		ble_face_rect(buffer, 60, 27, 9, 2);
	}
	return true;
}

static inline int ble_face_triangle(uint32_t phase, uint32_t period, int amplitude)
{
	uint32_t value = phase % period;

	if (value > period / 2) {
		value = period - value;
	}
	return (int)(value * (uint32_t)(4 * amplitude) / period) - amplitude;
}

/* State is generated on NU87; NU40 never advances a local animation clock. */
static inline struct ble_face ble_face_demo(uint32_t now_ms)
{
	struct ble_face face;
	uint32_t blink = now_ms % 2800;

	face.expression = (now_ms / 4000) % BLE_FACE_EXPRESSIONS;
	face.gaze_x = ble_face_triangle(now_ms, 3600, 8);
	face.gaze_y = ble_face_triangle(now_ms, 5200, 4);
	face.openness = blink < 180 ? 0 : blink < 360 ? 7 : 16;
	face.mouth = face.expression == BLE_FACE_SURPRISED ? 8 :
		     face.expression == BLE_FACE_CURIOUS && (now_ms / 600) % 2 ? 3 : 0;
	return face;
}

#endif /* BLE_FACE_H_ */
