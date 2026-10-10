/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../smp_relay.h"

int main(void)
{
	const uint8_t request[] = {2, 0, 0, 1, 0, 1, 42, 1, 0xa0};
	const uint8_t response[] = {3, 0, 0, 3, 0, 1, 42, 1, 0xa1, 0, 0};
	struct { uint32_t before; struct smp_relay_rx rx; uint32_t after; } guarded;
	guarded.before = 0x12345678;
	guarded.after = 0xabcdef12;
	struct smp_relay_rx *rx = &guarded.rx;

	for (size_t split = 1; split < sizeof(response); split++) {
		assert(smp_relay_begin(rx, request, sizeof(request)));
		assert(smp_relay_append(rx, response, split) == 0);
		assert(smp_relay_append(rx, response + split, sizeof(response) - split) == 1);
		assert(rx->complete && !rx->failed && rx->used == sizeof(response));
		assert(!memcmp(rx->data, response, sizeof(response)));
		assert(smp_relay_append(rx, response, sizeof(response)) == 0);
	}
	for (size_t field = 0; field < SMP_RELAY_HEADER; field++) {
		uint8_t wrong[sizeof(response)];
		memcpy(wrong, response, sizeof(wrong));
		wrong[field] ^= 1;
		assert(smp_relay_begin(rx, request, sizeof(request)));
		int result = smp_relay_append(rx, wrong, sizeof(wrong));
		assert(result != 1);
		assert(!rx->complete);
	}
	for (size_t len = 0; len < sizeof(request); len++) {
		assert(!smp_relay_begin(rx, request, len));
	}
	assert(!smp_relay_begin(rx, NULL, sizeof(request)));
	uint8_t large[SMP_RELAY_MAX + 1] = {3, 0, 1, 0xff, 0, 1, 42, 1};
	assert(smp_relay_begin(rx, request, sizeof(request)));
	assert(smp_relay_append(rx, large, sizeof(large)) == -1);
	assert(smp_relay_begin(rx, request, sizeof(request)));
	assert(smp_relay_append(rx, large, SMP_RELAY_HEADER) == -1);
	assert(smp_relay_begin(rx, request, sizeof(request)));
	assert(smp_relay_append(rx, NULL, 1) == -1);
	assert(guarded.before == 0x12345678 && guarded.after == 0xabcdef12);
	puts("PASS bounded fragmented SMP responses and request correlation");
	return 0;
}
