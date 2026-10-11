/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../src/voice.c"
#include "../src/alarm.c"

static struct bt_conn connection = {.references = 1, .mtu = 247};
static struct bt_gatt_attr audio_attribute, alarm_attribute;

static ssize_t arm(uint32_t session)
{
	uint8_t bytes[6] = {1, 1};
	voice_put32(bytes + 2, session);
	return voice_control(&connection, NULL, bytes, sizeof(bytes), 0, 0);
}

static void disconnect(void)
{
	voice_disconnected(&connection);
	alarm_disconnected(&connection);
	assert(connection.references == 1);
}

int main(void)
{
	voice_init(&audio_attribute);
	alarm_init(&alarm_attribute);
	connection.notify_enabled = true;
	/* Zephyr restores bonded CCC values before the application callback.
	 * Writing the identical CCC again does not trigger another change. */
	voice_subscription(&audio_attribute, BT_GATT_CCC_NOTIFY);
	alarm_subscription(&alarm_attribute, BT_GATT_CCC_NOTIFY);
	voice_connected(&connection);
	alarm_connected(&connection);
	assert(state.subscribed && link.subscribed && link.resend);
	assert(arm(101) == 6 && fake_dmic_starts == 0);
	assert(send_silence(101, state.transport.generation));
	send_audio(&audio_work);
	assert(fake_notify_calls == 1 && voice_u32(fake_notifications[0] + 4) == 101);

	/* A real alarm control callback must still emit its matching ACK after
	 * restoration. The worker is stepped without running its infinite loop. */
	uint8_t bytes[6] = {1, 0};
	voice_put32(bytes + 2, 42);
	assert(alarm_control(&connection, NULL, bytes, sizeof(bytes), 0, 0) == 6);
	struct alarm_request request;
	assert(k_msgq_get(&requests, &request, K_NO_WAIT) == 0);
	apply_alarm(request.on, request.sequence, 0);
	bt_conn_unref(request.conn);
	send_acks(&ack_work);
	assert(fake_notify_calls == 2);
	assert(fake_notifications[1][0] == 1 && fake_notifications[1][1] == 0);
	assert(voice_u32(fake_notifications[1] + 2) == 42);
	disconnect();

	/* A second bonded connection also works without a later CCC callback. */
	voice_connected(&connection);
	alarm_connected(&connection);
	assert(state.subscribed && link.subscribed && link.resend);
	assert(arm(102) == 6 && fake_dmic_starts == 0);
	disconnect();

	/* Fresh peers remain unsubscribed and cannot arm or request an ACK. */
	connection.notify_enabled = false;
	voice_connected(&connection);
	alarm_connected(&connection);
	assert(!state.subscribed && !link.subscribed && !link.resend);
	assert(arm(103) == BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF));
	assert(alarm_control(&connection, NULL, bytes, sizeof(bytes), 0, 0) ==
	       BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF));
	disconnect();
	return 0;
}
