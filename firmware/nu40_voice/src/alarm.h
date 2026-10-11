/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_ALARM_H
#define WALLABY_ALARM_H
#include <zephyr/bluetooth/gatt.h>
#define ALARM_UUID_CONTROL_VAL BT_UUID_128_ENCODE(0x90e2c005,0x3c0d,0x4ef3,0x9d78,0x574cd1a64087)
#define ALARM_UUID_ACK_VAL BT_UUID_128_ENCODE(0x90e2c006,0x3c0d,0x4ef3,0x9d78,0x574cd1a64087)
void alarm_init(const struct bt_gatt_attr *ack_attr);
bool alarm_is_active(void);
void alarm_connected(struct bt_conn *conn);
void alarm_disconnected(struct bt_conn *conn);
void alarm_subscription(const struct bt_gatt_attr *attr,uint16_t value);
ssize_t alarm_control(struct bt_conn *conn,const struct bt_gatt_attr *attr,
 const void *data,uint16_t length,uint16_t offset,uint8_t flags);
#endif
