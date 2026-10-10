/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NU40_SMP_RELAY_H_
#define NU40_SMP_RELAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SMP_RELAY_MAX 512U
#define SMP_RELAY_HEADER 8U

struct smp_relay_rx {
	uint8_t data[SMP_RELAY_MAX];
	size_t used, wanted;
	uint8_t op, seq, id;
	uint16_t group;
	bool active, complete, failed;
};

static inline uint16_t smp_relay_u16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

/* The bridge accepts SMP v1 read/write packets only, never arbitrary UART commands. */
static inline bool smp_relay_begin(struct smp_relay_rx *rx, const uint8_t *request, size_t len)
{
	memset(rx, 0, sizeof(*rx));
	if (request == NULL || len < SMP_RELAY_HEADER || len > SMP_RELAY_MAX ||
	    (request[0] != 0 && request[0] != 2) || request[1] != 0 ||
	    smp_relay_u16(request + 2) != len - SMP_RELAY_HEADER) {
		return false;
	}
	rx->op = request[0] + 1;
	rx->group = smp_relay_u16(request + 4);
	rx->seq = request[6];
	rx->id = request[7];
	rx->active = true;
	return true;
}

/* Return 1 for one complete matching response, 0 while waiting and -1 on corruption. */
static inline int smp_relay_append(struct smp_relay_rx *rx, const uint8_t *data, size_t len)
{
	if (!rx->active || rx->complete) {
		return 0;
	}
	if (rx->failed || data == NULL || len == 0 || len > sizeof(rx->data) - rx->used) {
		goto invalid;
	}
	memcpy(rx->data + rx->used, data, len);
	rx->used += len;
	if (rx->used < SMP_RELAY_HEADER) {
		return 0;
	}
	rx->wanted = SMP_RELAY_HEADER + smp_relay_u16(rx->data + 2);
	if (rx->wanted > sizeof(rx->data) || rx->used > rx->wanted ||
	    rx->data[0] != rx->op || rx->data[1] != 0 ||
	    smp_relay_u16(rx->data + 4) != rx->group || rx->data[6] != rx->seq ||
	    rx->data[7] != rx->id) {
		goto invalid;
	}
	rx->complete = rx->used == rx->wanted;
	return rx->complete ? 1 : 0;
invalid:
	rx->failed = true;
	return -1;
}

#endif
