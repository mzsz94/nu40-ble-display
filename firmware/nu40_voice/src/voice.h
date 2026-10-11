/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_VOICE_H
#define WALLABY_VOICE_H
#include <zephyr/bluetooth/gatt.h>
#include "voice_frame.h"
void voice_init(const struct bt_gatt_attr *audio_attr);
void voice_connected(struct bt_conn *conn);
void voice_disconnected(struct bt_conn *conn);
void voice_subscription(const struct bt_gatt_attr *attr, uint16_t value);
void voice_ota_pause(bool pause);
void voice_alarm_pause(bool pause);
bool voice_display_status(uint32_t *revision, char *text, size_t capacity);
/* Armed local recording controls take priority over decorative live scenes. */
bool voice_display_override(char *text, size_t capacity);
ssize_t voice_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                      const void *data, uint16_t length, uint16_t offset, uint8_t flags);
#endif
