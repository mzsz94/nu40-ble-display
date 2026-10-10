/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "../ble_face.h"

static void wire_vectors(void)
{
	struct ble_face face = {BLE_FACE_LOVE, -8, 4, 16, 8};
	struct ble_face parsed;
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	const uint8_t expected[] = {1, 3, 0x34, 0x12, 4, 0, 8, 16, 8, 0};
	uint16_t sequence = 0;
	struct ble_display_rx receiver;

	assert(ble_face_build(&face, 0x1234, frame) == sizeof(expected));
	assert(memcmp(frame, expected, sizeof(expected)) == 0);
	assert(ble_face_parse(frame, BLE_FACE_FRAME_SIZE, &sequence, &parsed));
	assert(sequence == 0x1234 && parsed.expression == BLE_FACE_LOVE);
	assert(parsed.gaze_x == -8 && parsed.gaze_y == 4 && parsed.openness == 16);
	assert(parsed.mouth == 8);
	ble_display_rx_reset(&receiver);
	assert(ble_display_receive(&receiver, frame, BLE_FACE_FRAME_SIZE, 0) == BLE_DISPLAY_INVALID);
	assert(ble_display_build_text("OK", 2, 0, 0, frame) == 8);
	assert(!ble_face_parse(frame, 8, &sequence, &parsed));
}

static void malformed(void)
{
	struct ble_face face = {BLE_FACE_CURIOUS, 0, 0, 16, 0}, parsed;
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	uint16_t sequence;

	memset(frame, 0, sizeof(frame));
	assert(ble_face_build(&face, 65535, frame));
	for (size_t length = 0; length <= sizeof(frame); length++) {
		assert(ble_face_parse(frame, length, &sequence, &parsed) ==
		       (length == BLE_FACE_FRAME_SIZE));
	}
	for (int index = 0; index < (int)BLE_FACE_FRAME_SIZE; index++) {
		uint8_t original = frame[index];

		for (int value = 0; value < 256; value++) {
			bool valid = index == 0 ? value == 1 : index == 1 ? value == 3 :
				     index == 4 ? value < 5 : index == 5 ? value <= 16 :
				     index == 6 ? value <= 8 : index == 7 ? value <= 16 :
				     index == 8 ? value <= 8 : index == 9 ? value == 0 : true;

			frame[index] = value;
			assert(ble_face_parse(frame, BLE_FACE_FRAME_SIZE, &sequence, &parsed) == valid);
		}
		frame[index] = original;
	}
	assert(!ble_face_parse(NULL, BLE_FACE_FRAME_SIZE, &sequence, &parsed));
	assert(!ble_face_parse(frame, BLE_FACE_FRAME_SIZE, NULL, &parsed));
	assert(!ble_face_parse(frame, BLE_FACE_FRAME_SIZE, &sequence, NULL));
}

static void output_preservation(void)
{
	struct ble_face sentinel = {BLE_FACE_HAPPY, 2, -1, 7, 3};
	struct ble_face parsed = sentinel;
	uint16_t sequence = 0xbeef;
	uint8_t frame[BLE_FACE_FRAME_SIZE] = {1, 3, 0, 0, 255, 8, 4, 16, 0, 0};
	uint8_t buffer[BLE_FACE_BUFFER_SIZE];

	assert(!ble_face_parse(frame, sizeof(frame), &sequence, &parsed));
	assert(sequence == 0xbeef && memcmp(&parsed, &sentinel, sizeof(parsed)) == 0);
	parsed.expression = 255;
	memset(buffer, 0xa5, sizeof(buffer));
	assert(!ble_face_render(&parsed, buffer));
	for (size_t i = 0; i < sizeof(buffer); i++) {
		assert(buffer[i] == 0xa5);
	}
	memset(frame, 0xa5, sizeof(frame));
	assert(!ble_face_build(&parsed, 1, frame));
	for (size_t i = 0; i < sizeof(frame); i++) {
		assert(frame[i] == 0xa5);
	}
	assert(!ble_face_build(NULL, 1, frame));
	assert(!ble_face_build(&sentinel, 1, NULL));
	assert(!ble_face_render(&sentinel, NULL));
}

static void render_boundaries(void)
{
	struct {
		uint8_t before[16];
		uint8_t pixels[BLE_FACE_BUFFER_SIZE];
		uint8_t after[16];
	} guarded;

	memset(&guarded, 0x5a, sizeof(guarded));
	for (int expression = 0; expression < (int)BLE_FACE_EXPRESSIONS; expression++) {
		for (int gaze = -8; gaze <= 8; gaze++) {
			for (int openness = 0; openness <= 16; openness++) {
				struct ble_face face = {expression, gaze, gaze / 2, openness, 8};
				bool nonempty = false;

				assert(ble_face_render(&face, guarded.pixels));
				for (size_t i = 0; i < sizeof(guarded.pixels); i++) {
					nonempty |= guarded.pixels[i] != 0;
				}
				assert(nonempty);
				for (size_t i = 0; i < sizeof(guarded.before); i++) {
					assert(guarded.before[i] == 0x5a && guarded.after[i] == 0x5a);
				}
			}
		}
	}
	ble_face_pixel(guarded.pixels, INT_MIN, INT_MIN, true);
	ble_face_pixel(guarded.pixels, INT_MAX, INT_MAX, true);
}

static void demo_states(void)
{
	uint8_t expressions = 0;
	bool closed = false, open = false;
	struct ble_face previous = ble_face_demo(0);
	uint8_t pixels[BLE_FACE_BUFFER_SIZE], old[BLE_FACE_BUFFER_SIZE];
	int changed = 0;

	assert(ble_face_render(&previous, old));
	for (uint32_t now = 0; now < 20000; now += 120) {
		struct ble_face face = ble_face_demo(now);

		assert(ble_face_valid(&face) && ble_face_render(&face, pixels));
		expressions |= 1U << face.expression;
		closed |= face.openness == 0;
		open |= face.openness == 16;
		changed += memcmp(pixels, old, sizeof(old)) != 0;
		memcpy(old, pixels, sizeof(old));
	}
	assert(expressions == 0x1f && closed && open && changed > 80);
	for (uint32_t now = UINT32_MAX - 10; now != 10; now++) {
		struct ble_face face = ble_face_demo(now);

		assert(ble_face_valid(&face));
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "preview")) {
		for (uint32_t now = 0; now < 20000; now += 120) {
			struct ble_face face = ble_face_demo(now);
			uint8_t pixels[BLE_FACE_BUFFER_SIZE];

			assert(ble_face_render(&face, pixels));
			assert(fwrite(pixels, sizeof(pixels), 1, stdout) == 1);
		}
		return 0;
	}
	if (!strcmp(argv[1], "wire_vectors")) {
		wire_vectors();
	} else if (!strcmp(argv[1], "malformed")) {
		malformed();
	} else if (!strcmp(argv[1], "output_preservation")) {
		output_preservation();
	} else if (!strcmp(argv[1], "render_boundaries")) {
		render_boundaries();
	} else if (!strcmp(argv[1], "demo_states")) {
		demo_states();
	} else {
		return 2;
	}
	printf("PASS %s\n", argv[1]);
	return 0;
}
