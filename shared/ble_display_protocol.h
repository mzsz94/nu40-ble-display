/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLE_DISPLAY_PROTOCOL_H_
#define BLE_DISPLAY_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* No larger MTU is needed: every write fits the default 20-byte ATT payload. */
#define BLE_DISPLAY_VERSION 1U
#define BLE_DISPLAY_TEXT 1U
#define BLE_DISPLAY_ACK 2U
#define BLE_DISPLAY_CHUNK 14U
#define BLE_DISPLAY_MAX_TEXT 84U
#define BLE_DISPLAY_FRAME_MAX 20U
#define BLE_DISPLAY_ACK_SIZE 5U
#define BLE_DISPLAY_TIMEOUT_MS 2000U
#define BLE_DISPLAY_COLUMNS 21U
#define BLE_DISPLAY_ROWS 4U

#define BLE_DISPLAY_UUID_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x90e2c000, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087)
#define BLE_DISPLAY_UUID_ACK_VAL \
	BT_UUID_128_ENCODE(0x90e2c001, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087)
#define BLE_DISPLAY_UUID_TEXT_VAL \
	BT_UUID_128_ENCODE(0x90e2c002, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087)

enum ble_display_result {
	BLE_DISPLAY_WAIT = 0,
	BLE_DISPLAY_COMPLETE = 1,
	BLE_DISPLAY_INVALID = -1,
	BLE_DISPLAY_ORDER = -2,
	BLE_DISPLAY_EXPIRED = -3,
};

enum ble_display_status {
	BLE_DISPLAY_OK = 0,
	BLE_DISPLAY_BAD_TEXT = 1,
	BLE_DISPLAY_OLED_ERROR = 2,
	BLE_DISPLAY_BUSY = 3,
};

struct ble_display_rx {
	char text[BLE_DISPLAY_MAX_TEXT + 1];
	uint32_t started_ms;
	uint16_t sequence;
	uint8_t total;
	uint8_t received;
	bool active;
};

static inline uint16_t ble_display_u16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static inline bool ble_display_valid_text(const char *text, size_t length)
{
	if (text == NULL || length == 0 || length > BLE_DISPLAY_MAX_TEXT) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		uint8_t c = (uint8_t)text[i];

		if (c != '\n' && (c < 32 || c > 126)) {
			return false;
		}
	}
	return true;
}

/* Render only complete messages that fit the 128x32 display's 6x8 font. */
static inline bool ble_display_split_lines(const char *text, size_t length,
					   char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1])
{
	size_t row = 0, column = 0;

	if (lines == NULL || !ble_display_valid_text(text, length)) {
		return false;
	}
	memset(lines, 0, BLE_DISPLAY_ROWS * (BLE_DISPLAY_COLUMNS + 1));
	for (size_t i = 0; i < length; i++) {
		if (text[i] == '\n') {
			row++;
			column = 0;
			continue;
		}
		if (column == BLE_DISPLAY_COLUMNS) {
			row++;
			column = 0;
		}
		if (row >= BLE_DISPLAY_ROWS) {
			return false;
		}
		lines[row][column++] = text[i];
	}
	return true;
}

/* Return frame length, or zero for invalid input. */
static inline size_t ble_display_build_text(const char *text, size_t length, uint16_t sequence,
					   uint8_t chunk, uint8_t out[BLE_DISPLAY_FRAME_MAX])
{
	size_t offset = (size_t)chunk * BLE_DISPLAY_CHUNK;
	size_t payload;

	if (out == NULL || !ble_display_valid_text(text, length) || offset >= length) {
		return 0;
	}
	payload = length - offset;
	if (payload > BLE_DISPLAY_CHUNK) {
		payload = BLE_DISPLAY_CHUNK;
	}
	out[0] = BLE_DISPLAY_VERSION;
	out[1] = BLE_DISPLAY_TEXT;
	out[2] = sequence & 0xff;
	out[3] = sequence >> 8;
	out[4] = chunk;
	out[5] = length;
	memcpy(out + 6, text + offset, payload);
	return 6 + payload;
}

static inline void ble_display_rx_reset(struct ble_display_rx *rx)
{
	memset(rx, 0, sizeof(*rx));
}

/* A complete text is exposed only after every in-order fragment is received. */
static inline enum ble_display_result ble_display_receive(struct ble_display_rx *rx,
							  const uint8_t *frame, size_t length,
							  uint32_t now_ms)
{
	size_t offset, payload;
	uint16_t sequence;
	uint8_t total, chunk;

	if (rx == NULL) {
		return BLE_DISPLAY_INVALID;
	}
	if (frame == NULL || length < 7 || length > BLE_DISPLAY_FRAME_MAX ||
	    frame[0] != BLE_DISPLAY_VERSION || frame[1] != BLE_DISPLAY_TEXT) {
		rx->active = false;
		return BLE_DISPLAY_INVALID;
	}
	sequence = ble_display_u16(frame + 2);
	chunk = frame[4];
	total = frame[5];
	offset = (size_t)chunk * BLE_DISPLAY_CHUNK;
	if (total == 0 || total > BLE_DISPLAY_MAX_TEXT || offset >= total) {
		rx->active = false;
		return BLE_DISPLAY_INVALID;
	}
	payload = total - offset;
	if (payload > BLE_DISPLAY_CHUNK) {
		payload = BLE_DISPLAY_CHUNK;
	}
	if (length != 6 + payload ||
	    !ble_display_valid_text((const char *)frame + 6, payload)) {
		rx->active = false;
		return BLE_DISPLAY_INVALID;
	}
	if (chunk == 0) {
		rx->sequence = sequence;
		rx->total = total;
		rx->received = 0;
		rx->started_ms = now_ms;
		rx->active = true;
	} else {
		if (!rx->active || rx->sequence != sequence || rx->total != total ||
		    rx->received != offset) {
			rx->active = false;
			return BLE_DISPLAY_ORDER;
		}
		if ((uint32_t)(now_ms - rx->started_ms) > BLE_DISPLAY_TIMEOUT_MS) {
			rx->active = false;
			return BLE_DISPLAY_EXPIRED;
		}
	}
	memcpy(rx->text + offset, frame + 6, payload);
	rx->received += payload;
	if (rx->received == rx->total) {
		rx->text[rx->total] = '\0';
		rx->active = false;
		return BLE_DISPLAY_COMPLETE;
	}
	return BLE_DISPLAY_WAIT;
}

static inline void ble_display_build_ack(uint16_t sequence, uint8_t status,
					 uint8_t out[BLE_DISPLAY_ACK_SIZE])
{
	out[0] = BLE_DISPLAY_VERSION;
	out[1] = BLE_DISPLAY_ACK;
	out[2] = sequence & 0xff;
	out[3] = sequence >> 8;
	out[4] = status;
}

static inline bool ble_display_parse_ack(const uint8_t *frame, size_t length,
					 uint16_t *sequence, uint8_t *status)
{
	if (frame == NULL || length != BLE_DISPLAY_ACK_SIZE ||
	    frame[0] != BLE_DISPLAY_VERSION || frame[1] != BLE_DISPLAY_ACK ||
	    frame[4] > BLE_DISPLAY_BUSY || sequence == NULL || status == NULL) {
		return false;
	}
	*sequence = ble_display_u16(frame + 2);
	*status = frame[4];
	return true;
}

#endif /* BLE_DISPLAY_PROTOCOL_H_ */
