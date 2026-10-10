/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>

#include "../ble_display_protocol.h"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

_Static_assert(BLE_DISPLAY_MAX_TEXT == 84, "text limit is a wire contract");
_Static_assert(BLE_DISPLAY_CHUNK == 14, "chunk size is a wire contract");
_Static_assert(BLE_DISPLAY_FRAME_MAX == 20, "default ATT payload limit");
_Static_assert(BLE_DISPLAY_ACK_SIZE == 5, "ACK size is a wire contract");
_Static_assert(BLE_DISPLAY_COLUMNS == 21, "6x8 font columns on the OLED");
_Static_assert(BLE_DISPLAY_ROWS == 4, "6x8 font rows on the OLED");

static void make_text(char *text, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		text[i] = i % 17 == 0 ? '\n' : (char)(' ' + i % 95);
	}
	text[length] = '\0';
}

static size_t make_frame(size_t total, uint16_t sequence, uint8_t chunk, uint8_t *frame)
{
	char text[BLE_DISPLAY_MAX_TEXT + 1];
	size_t length;

	make_text(text, total);
	length = ble_display_build_text(text, total, sequence, chunk, frame);
	CHECK(length >= 7 && length <= BLE_DISPLAY_FRAME_MAX);
	return length;
}

static void begin_message(struct ble_display_rx *rx, size_t total)
{
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	size_t length = make_frame(total, 0x1234, 0, frame);

	ble_display_rx_reset(rx);
	CHECK(ble_display_receive(rx, frame, length, 100) == BLE_DISPLAY_WAIT);
	CHECK(rx->active && rx->received == BLE_DISPLAY_CHUNK);
}

static void wire_vectors(void)
{
	const uint8_t expected_text[] = {1, 1, 0x34, 0x12, 0, 3, 'A', '\n', '~'};
	const uint8_t expected_ack[] = {1, 2, 0xef, 0xbe, 3};
	const uint8_t integer[] = {0x34, 0x12};
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	uint8_t ack[BLE_DISPLAY_ACK_SIZE];

	CHECK(ble_display_u16(integer) == 0x1234);
	CHECK(ble_display_build_text("A\n~", 3, 0x1234, 0, frame) == sizeof(expected_text));
	CHECK(memcmp(frame, expected_text, sizeof(expected_text)) == 0);
	ble_display_build_ack(0xbeef, BLE_DISPLAY_BUSY, ack);
	CHECK(memcmp(ack, expected_ack, sizeof(expected_ack)) == 0);
}

static void text_validation(void)
{
	char text[BLE_DISPLAY_MAX_TEXT + 2];

	make_text(text, BLE_DISPLAY_MAX_TEXT + 1);
	CHECK(ble_display_valid_text(text, BLE_DISPLAY_MAX_TEXT));
	CHECK(!ble_display_valid_text(text, BLE_DISPLAY_MAX_TEXT + 1));
	CHECK(!ble_display_valid_text(text, 0));
	CHECK(!ble_display_valid_text(NULL, 1));
	for (unsigned int value = 0; value <= UINT8_MAX; value++) {
		unsigned char byte = (unsigned char)value;
		bool allowed = value == '\n' || (value >= 32 && value <= 126);

		CHECK(ble_display_valid_text((const char *)&byte, 1) == allowed);
	}
}

static void layout_wrap_boundaries(void)
{
	char text[BLE_DISPLAY_MAX_TEXT + 1];
	struct {
		uint8_t before;
		char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];
		uint8_t after;
	} guarded;

	for (size_t length = 1; length <= BLE_DISPLAY_MAX_TEXT; length++) {
		for (size_t i = 0; i < length; i++) {
			text[i] = (char)('A' + i % 26);
		}
		memset(&guarded, 0xa5, sizeof(guarded));
		guarded.after = 0x5a;
		CHECK(ble_display_split_lines(text, length, guarded.lines));
		CHECK(guarded.before == 0xa5 && guarded.after == 0x5a);
		for (size_t row = 0; row < BLE_DISPLAY_ROWS; row++) {
			size_t start = row * BLE_DISPLAY_COLUMNS;
			size_t count = length > start ? length - start : 0;

			if (count > BLE_DISPLAY_COLUMNS) {
				count = BLE_DISPLAY_COLUMNS;
			}
			CHECK(strlen(guarded.lines[row]) == count);
			CHECK(memcmp(guarded.lines[row], text + start, count) == 0);
		}
	}
}

static void layout_explicit_newlines(void)
{
	char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];
	const char *full_row_lf = "ABCDEFGHIJKLMNOPQRSTU\nZ";
	const char *four_rows = "first\nsecond\nthird\nfourth";
	const char *trailing_lf = "a\nb\nc\nd\n";
	const char *fifth_row = "a\nb\nc\nd\nx";
	const char *empty_rows = "\n\n\nX";

	CHECK(ble_display_split_lines(full_row_lf, strlen(full_row_lf), lines));
	CHECK(strcmp(lines[0], "ABCDEFGHIJKLMNOPQRSTU") == 0);
	CHECK(strcmp(lines[1], "Z") == 0);
	CHECK(lines[2][0] == '\0' && lines[3][0] == '\0');
	CHECK(ble_display_split_lines(four_rows, strlen(four_rows), lines));
	CHECK(strcmp(lines[0], "first") == 0 && strcmp(lines[1], "second") == 0);
	CHECK(strcmp(lines[2], "third") == 0 && strcmp(lines[3], "fourth") == 0);
	CHECK(ble_display_split_lines(trailing_lf, strlen(trailing_lf), lines));
	CHECK(strcmp(lines[3], "d") == 0);
	CHECK(!ble_display_split_lines(fifth_row, strlen(fifth_row), lines));
	CHECK(ble_display_split_lines(empty_rows, strlen(empty_rows), lines));
	CHECK(lines[0][0] == '\0' && lines[1][0] == '\0' && lines[2][0] == '\0');
	CHECK(strcmp(lines[3], "X") == 0);
	CHECK(!ble_display_split_lines("\n\n\n\nX", 5, lines));
	CHECK(ble_display_split_lines("\n", 1, lines));
	for (size_t row = 0; row < BLE_DISPLAY_ROWS; row++) {
		CHECK(lines[row][0] == '\0');
	}
}

static void layout_invalid_input(void)
{
	char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];
	char text[BLE_DISPLAY_MAX_TEXT + 1];

	memset(text, 'X', sizeof(text));
	CHECK(!ble_display_split_lines(NULL, 1, lines));
	CHECK(!ble_display_split_lines("X", 1, NULL));
	CHECK(!ble_display_split_lines("", 0, lines));
	CHECK(!ble_display_split_lines(text, sizeof(text), lines));
	CHECK(!ble_display_split_lines("a\rb", 3, lines));
	CHECK(!ble_display_split_lines("\x7f", 1, lines));
	CHECK(!ble_display_split_lines("\xc3\xa9", 2, lines));
}

static void all_text_lengths(void)
{
	for (size_t total = 1; total <= BLE_DISPLAY_MAX_TEXT; total++) {
		struct ble_display_rx rx;
		char text[BLE_DISPLAY_MAX_TEXT + 1];
		size_t chunks = (total + BLE_DISPLAY_CHUNK - 1) / BLE_DISPLAY_CHUNK;

		make_text(text, total);
		ble_display_rx_reset(&rx);
		for (size_t chunk = 0; chunk < chunks; chunk++) {
			struct {
				uint8_t before;
				uint8_t frame[BLE_DISPLAY_FRAME_MAX];
				uint8_t after;
			} guarded = {.before = 0xa5, .after = 0x5a};
			size_t payload = total - chunk * BLE_DISPLAY_CHUNK;
			size_t length;
			enum ble_display_result expected;

			if (payload > BLE_DISPLAY_CHUNK) {
				payload = BLE_DISPLAY_CHUNK;
			}
			length = ble_display_build_text(text, total, 0xffff, (uint8_t)chunk,
						guarded.frame);
			CHECK(length == 6 + payload);
			CHECK(guarded.before == 0xa5 && guarded.after == 0x5a);
			expected = chunk + 1 == chunks ? BLE_DISPLAY_COMPLETE : BLE_DISPLAY_WAIT;
			CHECK(ble_display_receive(&rx, guarded.frame, length,
						 100 + (uint32_t)chunk) == expected);
			CHECK(rx.active == (expected == BLE_DISPLAY_WAIT));
		}
		CHECK(rx.sequence == 0xffff && rx.total == total && rx.received == total);
		CHECK(memcmp(rx.text, text, total + 1) == 0);
	}
}

static void builder_rejects_invalid_input(void)
{
	char text[BLE_DISPLAY_MAX_TEXT + 2];
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];

	make_text(text, BLE_DISPLAY_MAX_TEXT + 1);
	CHECK(ble_display_build_text(NULL, 1, 0, 0, frame) == 0);
	CHECK(ble_display_build_text(text, 1, 0, 0, NULL) == 0);
	CHECK(ble_display_build_text(text, 0, 0, 0, frame) == 0);
	CHECK(ble_display_build_text(text, BLE_DISPLAY_MAX_TEXT + 1, 0, 0, frame) == 0);
	CHECK(ble_display_build_text(text, BLE_DISPLAY_MAX_TEXT, 0, 6, frame) == 0);
	CHECK(ble_display_build_text(text, BLE_DISPLAY_MAX_TEXT, 0, UINT8_MAX, frame) == 0);
	CHECK(ble_display_build_text("a", 1, 0, 1, frame) == 0);
	CHECK(ble_display_build_text("a\rb", 3, 0, 0, frame) == 0);
}

static void expect_invalid(struct ble_display_rx *rx, const uint8_t *frame, size_t length)
{
	uint8_t continuation[BLE_DISPLAY_FRAME_MAX];
	size_t continuation_length = make_frame(28, 0x1234, 1, continuation);

	begin_message(rx, 28);
	CHECK(ble_display_receive(rx, frame, length, 101) == BLE_DISPLAY_INVALID);
	CHECK(!rx->active);
	CHECK(ble_display_receive(rx, continuation, continuation_length, 102) == BLE_DISPLAY_ORDER);
}

static void malformed_frames_cancel_assembly(void)
{
	struct ble_display_rx rx;
	uint8_t frame[BLE_DISPLAY_FRAME_MAX + 1];
	size_t length = make_frame(28, 0x1234, 1, frame);

	CHECK(ble_display_receive(NULL, frame, length, 100) == BLE_DISPLAY_INVALID);
	expect_invalid(&rx, NULL, length);
	for (size_t size = 0; size < 7; size++) {
		expect_invalid(&rx, frame, size);
	}
	expect_invalid(&rx, frame, BLE_DISPLAY_FRAME_MAX + 1);
	expect_invalid(&rx, frame, length - 1);
	for (size_t field = 0; field < 2; field++) {
		uint8_t saved = frame[field];

		frame[field] = 0xff;
		expect_invalid(&rx, frame, length);
		frame[field] = saved;
	}
	frame[5] = 0;
	expect_invalid(&rx, frame, length);
	frame[5] = BLE_DISPLAY_MAX_TEXT + 1;
	expect_invalid(&rx, frame, length);
	frame[5] = 28;
	frame[4] = 2;
	expect_invalid(&rx, frame, length);
	frame[4] = 1;
	frame[6] = 0;
	expect_invalid(&rx, frame, length);
	length = make_frame(15, 0x1234, 1, frame);
	CHECK(length == 7);
	expect_invalid(&rx, frame, length + 1);
}

static void fragment_order_and_identity(void)
{
	struct ble_display_rx rx;
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	size_t length = make_frame(42, 0x1234, 1, frame);

	ble_display_rx_reset(&rx);
	CHECK(ble_display_receive(&rx, frame, length, 100) == BLE_DISPLAY_ORDER);
	begin_message(&rx, 42);
	length = make_frame(42, 0x1234, 2, frame);
	CHECK(ble_display_receive(&rx, frame, length, 101) == BLE_DISPLAY_ORDER);
	begin_message(&rx, 42);
	length = make_frame(42, 0x1235, 1, frame);
	CHECK(ble_display_receive(&rx, frame, length, 101) == BLE_DISPLAY_ORDER);
	CHECK(!rx.active);
	begin_message(&rx, 42);
	length = make_frame(28, 0x1234, 1, frame);
	CHECK(ble_display_receive(&rx, frame, length, 101) == BLE_DISPLAY_ORDER);
	begin_message(&rx, 42);
	length = make_frame(42, 0x1234, 1, frame);
	CHECK(ble_display_receive(&rx, frame, length, 101) == BLE_DISPLAY_WAIT);
	CHECK(ble_display_receive(&rx, frame, length, 102) == BLE_DISPLAY_ORDER);
	CHECK(!rx.active);
}

static void reset_and_restart(void)
{
	struct ble_display_rx rx;
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	size_t length;

	begin_message(&rx, BLE_DISPLAY_MAX_TEXT);
	ble_display_rx_reset(&rx);
	CHECK(!rx.active && rx.sequence == 0 && rx.total == 0 && rx.received == 0);
	CHECK(rx.started_ms == 0);
	for (size_t i = 0; i < sizeof(rx.text); i++) {
		CHECK(rx.text[i] == '\0');
	}
	begin_message(&rx, BLE_DISPLAY_MAX_TEXT);
	length = make_frame(28, 0x4321, 0, frame);
	CHECK(ble_display_receive(&rx, frame, length, 5000) == BLE_DISPLAY_WAIT);
	CHECK(rx.sequence == 0x4321 && rx.total == 28 && rx.started_ms == 5000);
	CHECK(rx.received == BLE_DISPLAY_CHUNK);
	CHECK(ble_display_receive(&rx, frame, length, 6000) == BLE_DISPLAY_WAIT);
	CHECK(rx.started_ms == 6000 && rx.received == BLE_DISPLAY_CHUNK);
	length = make_frame(28, 0x4321, 1, frame);
	CHECK(ble_display_receive(&rx, frame, length, 6001) == BLE_DISPLAY_COMPLETE);
	length = ble_display_build_text("Z", 1, 0, 0, frame);
	CHECK(ble_display_receive(&rx, frame, length, 6002) == BLE_DISPLAY_COMPLETE);
	CHECK(strcmp(rx.text, "Z") == 0 && rx.received == 1 && rx.sequence == 0);
}

static void duplicates_after_completion(void)
{
	struct ble_display_rx rx;
	uint8_t frame[BLE_DISPLAY_FRAME_MAX];
	char completed[BLE_DISPLAY_MAX_TEXT + 1];
	size_t length;

	begin_message(&rx, 28);
	length = make_frame(28, 0x1234, 1, frame);
	CHECK(ble_display_receive(&rx, frame, length, 101) == BLE_DISPLAY_COMPLETE);
	memcpy(completed, rx.text, sizeof(completed));
	CHECK(ble_display_receive(&rx, frame, length, 102) == BLE_DISPLAY_ORDER);
	CHECK(!rx.active && memcmp(rx.text, completed, sizeof(completed)) == 0);

	/* Chunk zero always starts a transfer; sequence deduplication is an app policy. */
	length = make_frame(28, 0x1234, 0, frame);
	CHECK(ble_display_receive(&rx, frame, length, 103) == BLE_DISPLAY_WAIT);
	length = make_frame(28, 0x1234, 1, frame);
	CHECK(ble_display_receive(&rx, frame, length, 104) == BLE_DISPLAY_COMPLETE);
	length = ble_display_build_text("X", 1, 0x1234, 0, frame);
	CHECK(ble_display_receive(&rx, frame, length, 105) == BLE_DISPLAY_COMPLETE);
	CHECK(ble_display_receive(&rx, frame, length, 106) == BLE_DISPLAY_COMPLETE);
}

static void timeout_and_clock_wrap(void)
{
	struct ble_display_rx rx;
	uint8_t first[BLE_DISPLAY_FRAME_MAX], last[BLE_DISPLAY_FRAME_MAX];
	size_t first_length = make_frame(28, 0x1234, 0, first);
	size_t last_length = make_frame(28, 0x1234, 1, last);
	const uint32_t starts[] = {100, UINT32_MAX - 100};

	for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
		uint32_t start = starts[i];
		uint32_t deadline = start + BLE_DISPLAY_TIMEOUT_MS;

		ble_display_rx_reset(&rx);
		CHECK(ble_display_receive(&rx, first, first_length, start) == BLE_DISPLAY_WAIT);
		CHECK(ble_display_receive(&rx, last, last_length, deadline) ==
		      BLE_DISPLAY_COMPLETE);
		CHECK(ble_display_receive(&rx, first, first_length, start) == BLE_DISPLAY_WAIT);
		CHECK(ble_display_receive(&rx, last, last_length, deadline + 1) ==
		      BLE_DISPLAY_EXPIRED);
		CHECK(!rx.active);
		CHECK(ble_display_receive(&rx, last, last_length, deadline + 2) ==
		      BLE_DISPLAY_ORDER);
		CHECK(ble_display_receive(&rx, first, first_length, deadline + 3) ==
		      BLE_DISPLAY_WAIT);
	}
}

static void ack_validation(void)
{
	uint8_t ack[BLE_DISPLAY_ACK_SIZE + 1];
	uint16_t sequence;
	uint8_t status;
	const uint16_t sequences[] = {0, 0x80ff, UINT16_MAX};

	for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
		for (uint8_t code = BLE_DISPLAY_OK; code <= BLE_DISPLAY_BUSY; code++) {
			ble_display_build_ack(sequences[i], code, ack);
			CHECK(ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, &sequence, &status));
			CHECK(sequence == sequences[i] && status == code);
		}
	}
	ble_display_build_ack(0x1234, BLE_DISPLAY_OK, ack);
	sequence = 0xabcd;
	status = 0xfe;
	for (size_t length = 0; length <= sizeof(ack); length++) {
		if (length != BLE_DISPLAY_ACK_SIZE) {
			CHECK(!ble_display_parse_ack(ack, length, &sequence, &status));
		}
	}
	CHECK(!ble_display_parse_ack(NULL, BLE_DISPLAY_ACK_SIZE, &sequence, &status));
	CHECK(!ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, NULL, &status));
	CHECK(!ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, &sequence, NULL));
	ack[0] = 2;
	CHECK(!ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, &sequence, &status));
	ack[0] = BLE_DISPLAY_VERSION;
	ack[1] = BLE_DISPLAY_TEXT;
	CHECK(!ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, &sequence, &status));
	ack[1] = BLE_DISPLAY_ACK;
	ack[4] = BLE_DISPLAY_BUSY + 1;
	CHECK(!ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, &sequence, &status));
	ack[4] = UINT8_MAX;
	CHECK(!ble_display_parse_ack(ack, BLE_DISPLAY_ACK_SIZE, &sequence, &status));
	CHECK(sequence == 0xabcd && status == 0xfe);
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} tests[] = {
		{"wire_vectors", wire_vectors},
		{"text_validation", text_validation},
		{"layout_wrap_boundaries", layout_wrap_boundaries},
		{"layout_explicit_newlines", layout_explicit_newlines},
		{"layout_invalid_input", layout_invalid_input},
		{"all_text_lengths", all_text_lengths},
		{"builder_rejects_invalid_input", builder_rejects_invalid_input},
		{"malformed_frames_cancel_assembly", malformed_frames_cancel_assembly},
		{"fragment_order_and_identity", fragment_order_and_identity},
		{"reset_and_restart", reset_and_restart},
		{"duplicates_after_completion", duplicates_after_completion},
		{"timeout_and_clock_wrap", timeout_and_clock_wrap},
		{"ack_validation", ack_validation},
	};
	unsigned int executed = 0;

	CHECK(argc <= 2);
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (argc == 1 || strcmp(argv[1], tests[i].name) == 0) {
			tests[i].run();
			printf("PASS %s\n", tests[i].name);
			executed++;
		}
	}
	CHECK(executed > 0);
	return EXIT_SUCCESS;
}
