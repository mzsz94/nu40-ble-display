/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_VOICE_STATUS_H
#define WALLABY_VOICE_STATUS_H
/* Local UI state only; the BLE audio protocol is unchanged. */
enum voice_status {
	VOICE_OFF,
	VOICE_ARMED,
	VOICE_RECORDING,
	VOICE_READY,
	VOICE_SENDING,
	VOICE_SENT,
	VOICE_ERROR,
};
#endif
