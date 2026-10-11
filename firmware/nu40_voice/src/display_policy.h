/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_DISPLAY_POLICY_H
#define WALLABY_DISPLAY_POLICY_H
#include "wallaby_graphics.h"
#include "voice_status.h"
#include "ble_display_protocol.h"

#define WALLABY_TEXT_HOLD_MS 8000U
#define WALLABY_VOICE_SENT_MS 3000U
#define WALLABY_VOICE_ERROR_MS 8000U

static inline bool wallaby_text_holding(bool requested, uint32_t received_at, uint32_t now)
{
	return requested && (uint32_t)(now - received_at) < WALLABY_TEXT_HOLD_MS;
}

static inline bool wallaby_scene_priority(const struct wallaby_scene *scene, bool expired)
{
	return expired || scene->state == 'X' || scene->state == 'A' || scene->state == 'B';
}

/* The host intentionally pauses ordinary WG1 sends during its 8-second answer
 * hold. Ageing the previous sample must not erase that answer after 3 seconds.
 * Explicit warning/stale scenes and the local alarm still take priority. */
static inline bool wallaby_display_uses_scene(bool have_scene, const struct wallaby_scene *scene,
					    bool local_alarm, bool text_hold, bool scene_after_text)
{
	return local_alarm || (have_scene && (!text_hold ||
		(scene_after_text && wallaby_scene_priority(scene, false))));
}

static inline const char *wallaby_voice_label(enum voice_status status, uint32_t age)
{
	switch (status) {
	case VOICE_ARMED: return "MIC READY";
	case VOICE_RECORDING: return "MIC REC";
	case VOICE_READY: return "BTN3 SEND";
	case VOICE_SENDING: return "MIC SEND";
	case VOICE_SENT: return age < WALLABY_VOICE_SENT_MS ? "MIC SENT" : NULL;
	case VOICE_ERROR: return age < WALLABY_VOICE_ERROR_MS ? "MIC ERROR" : NULL;
	default: return NULL;
	}
}

static inline bool wallaby_display_text(const char *text, enum voice_status voice,
				       uint32_t voice_age,
				       char lines[BLE_DISPLAY_ROWS][BLE_DISPLAY_COLUMNS + 1])
{
	if (!text || !ble_display_split_lines(text, strlen(text), lines)) return false;
	const char *label = wallaby_voice_label(voice, voice_age);
	/* The regular IMU text uses three rows. Keep all supplied payload rows,
	 * including a fourth row in longer answers, and use only spare space. */
	if (label && lines[BLE_DISPLAY_ROWS - 1][0] == '\0') {
		snprintf(lines[BLE_DISPLAY_ROWS - 1], BLE_DISPLAY_COLUMNS + 1, "%s", label);
	}
	return true;
}

static inline bool wallaby_display_render(const struct wallaby_scene *scene, bool expired,
					 uint32_t now, bool local_alarm, enum voice_status voice,
					 uint32_t voice_age, uint8_t pixels[WALLABY_PIXELS],
					 char labels[4][WALLABY_LABEL_COLUMNS + 1])
{
	if (!scene) return false;
	struct wallaby_scene effective = *scene;
	if (local_alarm) effective.state = 'B';
	if (!wallaby_scene_render(&effective, expired, now, pixels, labels)) return false;
	const char *label = wallaby_voice_label(voice, voice_age);
	if (label && !wallaby_scene_priority(&effective, expired)) {
		/* Preserve the measured face/horizon, warning title and R/P/T values.
		 * Only the sample-number row is temporarily used for microphone state. */
		snprintf(labels[3], WALLABY_LABEL_COLUMNS + 1, "%s", label);
	}
	return true;
}
#endif
