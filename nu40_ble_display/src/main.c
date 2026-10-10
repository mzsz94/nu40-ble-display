/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <hal/nrf_power.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "ble_display_protocol.h"

static const struct device *const oled = DEVICE_DT_GET(DT_NODELABEL(kit_oled));
static const struct gpio_dt_spec amp = GPIO_DT_SPEC_GET(DT_NODELABEL(kit_amp), gpios);
static const struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(BLE_DISPLAY_UUID_SERVICE_VAL);
static const struct bt_uuid_128 ack_uuid = BT_UUID_INIT_128(BLE_DISPLAY_UUID_ACK_VAL);
static const struct bt_uuid_128 text_uuid = BT_UUID_INIT_128(BLE_DISPLAY_UUID_TEXT_VAL);
static const struct bt_data advertising[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BLE_DISPLAY_UUID_SERVICE_VAL),
};
static const struct bt_data scan_response[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

struct received_text {
	uint32_t session;
	uint16_t sequence;
	char text[BLE_DISPLAY_MAX_TEXT + 1];
};

K_MSGQ_DEFINE(messages, sizeof(struct received_text), 2, 4);
K_MUTEX_DEFINE(link_lock);
static struct ble_display_rx receiver;
static struct bt_conn *peer;
static uint32_t session;
static atomic_t link_revision;
static bool oled_ready;
static uint32_t received_count, oled_writes, ack_count, error_count;

static bool ack_subscribed(struct bt_conn *conn);

static ssize_t receive_text(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buffer, uint16_t length, uint16_t offset, uint8_t flags)
{
	struct received_text message;
	enum ble_display_result result;
	int queued = 0;

	ARG_UNUSED(attr);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}
	if (!ack_subscribed(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	result = ble_display_receive(&receiver, buffer, length, k_uptime_get_32());
	if (result == BLE_DISPLAY_COMPLETE) {
		message.session = session;
		message.sequence = receiver.sequence;
		memcpy(message.text, receiver.text, sizeof(message.text));
		queued = k_msgq_put(&messages, &message, K_NO_WAIT);
	}
	k_mutex_unlock(&link_lock);
	if (result < 0 || queued != 0) {
		printk("LINK|RX rejected=%d queue_result=%d\n", result, queued);
		return BT_GATT_ERR(queued != 0 ? BT_ATT_ERR_INSUFFICIENT_RESOURCES :
				   BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	return length;
}

static void subscription_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	printk("LINK|ACK subscribed=%u\n", value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(display_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&ack_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(subscription_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&text_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, receive_text, NULL)
);

static bool ack_subscribed(struct bt_conn *conn)
{
	return bt_gatt_is_subscribed(conn, &display_service.attrs[2], BT_GATT_CCC_NOTIFY);
}

static int draw_text(const char *text)
{
	char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1];
	int ret;

	if (!ble_display_split_lines(text, strlen(text), lines)) {
		return -E2BIG;
	}
	if (!oled_ready) {
		return -ENODEV;
	}
	ret = cfb_framebuffer_clear(oled, false);
	for (size_t i = 0; i < BLE_DISPLAY_ROWS && ret == 0; i++) {
		ret = cfb_print(oled, lines[i], 0, i * 8);
	}
	if (ret == 0) {
		ret = cfb_framebuffer_finalize(oled);
	}
	if (ret == 0) {
		oled_writes++;
	}
	return ret;
}

static void connected(struct bt_conn *conn, uint8_t error)
{
	printk("LINK|BLE connected=%u hci_error=%u\n", error == 0, error);
	if (error != 0) {
		return;
	}
	k_mutex_lock(&link_lock, K_FOREVER);
	peer = bt_conn_ref(conn);
	session++;
	ble_display_rx_reset(&receiver);
	k_mutex_unlock(&link_lock);
	atomic_inc(&link_revision);
}

static void restart_advertising(struct k_work *work);
K_WORK_DEFINE(advertising_work, restart_advertising);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("LINK|BLE disconnected=1 reason=%u\n", reason);
	k_mutex_lock(&link_lock, K_FOREVER);
	if (peer == conn) {
		bt_conn_unref(peer);
		peer = NULL;
	}
	session++;
	ble_display_rx_reset(&receiver);
	k_mutex_unlock(&link_lock);
	atomic_inc(&link_revision);
}

static void recycled(void)
{
	k_work_submit(&advertising_work);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static void restart_advertising(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);
	ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising, ARRAY_SIZE(advertising),
			      scan_response, ARRAY_SIZE(scan_response));
	printk("LINK|BLE advertising_result=%d name=%s\n", ret, CONFIG_BT_DEVICE_NAME);
}

static void process_message(const struct received_text *message)
{
	struct bt_conn *conn = NULL;
	uint8_t ack[BLE_DISPLAY_ACK_SIZE];
	uint8_t status;
	int rendered, notified;

	k_mutex_lock(&link_lock, K_FOREVER);
	if (peer != NULL && message->session == session) {
		conn = bt_conn_ref(peer);
	}
	k_mutex_unlock(&link_lock);
	if (conn == NULL) {
		return;
	}
	rendered = draw_text(message->text);
	status = rendered == 0 ? BLE_DISPLAY_OK :
		 rendered == -E2BIG ? BLE_DISPLAY_BAD_TEXT : BLE_DISPLAY_OLED_ERROR;
	ble_display_build_ack(message->sequence, status, ack);
	notified = bt_gatt_notify(conn, &display_service.attrs[2], ack, sizeof(ack));
	bt_conn_unref(conn);
	received_count++;
	if (rendered == 0 && notified == 0) {
		ack_count++;
	} else {
		error_count++;
	}
	/* oled_result confirms the driver transfer; visible text needs observation. */
	printk("LINK|RX seq=%u bytes=%u oled_result=%d ack_result=%d visible=unverified\n",
	       message->sequence, (unsigned int)strlen(message->text), rendered, notified);
}

static void report_status(void)
{
	bool connected_now;

	k_mutex_lock(&link_lock, K_FOREVER);
	connected_now = peer != NULL;
	k_mutex_unlock(&link_lock);
	printk("LINK|STATUS role=display connected=%u oled_ready=%u rx=%u oled_writes=%u "
	       "ack_queued=%u errors=%u\n", connected_now, oled_ready, received_count,
	       oled_writes, ack_count, error_count);
}

static void enter_bootloader(void)
{
	printk("LINK|CMD uf2 accepted=1\n");
	k_msleep(50);
	nrf_power_gpregret_set(NRF_POWER, 0U, 0x57U);
	sys_reboot(SYS_REBOOT_COLD);
}

static void poll_console(const struct device *console)
{
	static char command[16];
	static size_t used;
	static bool overflow;
	uint32_t baudrate;
	unsigned char c;

	if (uart_line_ctrl_get(console, UART_LINE_CTRL_BAUD_RATE, &baudrate) == 0 &&
	    baudrate == 1200) {
		enter_bootloader();
	}
	for (int i = 0; i < 64 && uart_poll_in(console, &c) == 0; i++) {
		if (c == '\r' || c == '\n') {
			command[used] = '\0';
			if (!overflow && strcmp(command, "uf2") == 0) {
				enter_bootloader();
			} else if (!overflow && strcmp(command, "status") == 0) {
				report_status();
			} else if (used || overflow) {
				printk("LINK|CMD supported=status,uf2\n");
			}
			used = 0;
			overflow = false;
		} else if (used < sizeof(command) - 1) {
			command[used++] = c;
		} else {
			overflow = true;
		}
	}
}

int main(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	struct received_text message;
	atomic_val_t seen_revision = -1;
	int64_t report_at = 0;
	int ret;

	(void)gpio_pin_configure_dt(&amp, GPIO_OUTPUT_INACTIVE);
	ret = device_is_ready(oled) ? cfb_framebuffer_init(oled) : -ENODEV;
	if (ret == 0) {
		ret = cfb_framebuffer_set_font(oled, 0);
	}
	if (ret == 0) {
		ret = display_blanking_off(oled);
	}
	oled_ready = ret == 0;
	printk("LINK|BOOT role=display firmware=1 oled_init=%d protocol=1\n", ret);
	(void)draw_text("NU40 BLE DISPLAY\nWaiting for NU87\nNo data received");
	ret = bt_enable(NULL);
	printk("LINK|BLE enable_result=%d\n", ret);
	if (ret == 0) {
		k_work_submit(&advertising_work);
	} else {
		(void)draw_text("BLE INIT ERROR\nCheck USB log");
	}
	while (true) {
		atomic_val_t revision = atomic_get(&link_revision);

		if (revision != seen_revision) {
			bool connected_now;

			k_mutex_lock(&link_lock, K_FOREVER);
			connected_now = peer != NULL;
			k_mutex_unlock(&link_lock);
			if (ret == 0) {
				(void)draw_text(connected_now ?
					"BLE CONNECTED\nWaiting for NU87 data" :
					"WAITING FOR NU87\nBLE disconnected\nNo current data");
			}
			seen_revision = revision;
		}
		if (k_msgq_get(&messages, &message, K_MSEC(20)) == 0) {
			process_message(&message);
		}
		poll_console(console);
		if (k_uptime_get() >= report_at) {
			report_status();
			report_at = k_uptime_get() + 2000;
		}
	}
	return 0;
}
