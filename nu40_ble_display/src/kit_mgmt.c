/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include "kit_io.h"

#if defined(CONFIG_MCUMGR)
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/app_version.h>
#include <zephyr/sys/util.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#define KIT_MGMT_GROUP 64

static int read_status(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct kit_status s;

	kit_snapshot(&s);
#define PUT_U(name, value) (zcbor_tstr_put_lit(zse, name) && zcbor_uint32_put(zse, value))
#define PUT_I(name, value) (zcbor_tstr_put_lit(zse, name) && zcbor_int32_put(zse, value))
	bool ok = PUT_U("schema", 1) && PUT_U("keys", s.keys_ready) &&
		  PUT_U("pressed", s.pressed) && PUT_U("seen", s.seen) &&
		  PUT_U("page", s.page) && PUT_U("rgb_mode", s.rgb_mode) &&
		  PUT_U("busy", s.busy) && PUT_U("mic_blocks", s.mic_blocks) &&
		  PUT_U("mic_errors", s.mic_errors) && PUT_U("peak", s.peak) &&
		  PUT_U("rms", s.rms) && PUT_U("samples", s.recorded_samples) &&
		  PUT_U("records", s.records) && PUT_U("beeps", s.beeps) &&
		  PUT_U("plays", s.plays) && PUT_U("sd_saves", s.sd_saves) &&
		  PUT_U("sd_reads", s.sd_reads) && PUT_I("mic_result", s.mic_result) &&
		  PUT_I("speaker_result", s.speaker_result) && PUT_I("rgb_result", s.rgb_result) &&
		  PUT_I("sd_result", s.sd_result) && PUT_I("audio_result", s.audio_result) &&
		  PUT_U("sd_mounted", s.sd_mounted) && zcbor_tstr_put_lit(zse, "file") &&
		  zcbor_tstr_put_term(zse, s.last_file, sizeof(s.last_file)) &&
		  zcbor_tstr_put_lit(zse, "version") && zcbor_tstr_put_lit(zse, APP_VERSION_STRING);
#undef PUT_U
#undef PUT_I
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int write_action(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs, *zse = ctxt->writer->zs;
	uint32_t action = 0, value = 0;
	size_t decoded = 0;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("action", zcbor_uint32_decode, &action),
		ZCBOR_MAP_DECODE_KEY_DECODER("value", zcbor_uint32_decode, &value),
	};

	if (zcbor_map_decode_bulk(zsd, fields, ARRAY_SIZE(fields), &decoded) != 0 ||
	    action < KIT_BEEP || action > KIT_RGB) {
		return MGMT_ERR_EINVAL;
	}
	int ret = kit_request((enum kit_action)action, value);

	if (ret != 0) {
		return ret == -EBUSY ? MGMT_ERR_EBUSY : MGMT_ERR_EINVAL;
	}
	bool ok = zcbor_tstr_put_lit(zse, "queued") && zcbor_uint32_put(zse, action) &&
		  zcbor_tstr_put_lit(zse, "value") && zcbor_uint32_put(zse, value);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static const struct mgmt_handler handlers[] = {
	[0] = {.mh_read = read_status},
	[1] = {.mh_write = write_action},
};
static struct mgmt_group group = {
	.mg_handlers = handlers,
	.mg_handlers_count = ARRAY_SIZE(handlers),
	.mg_group_id = KIT_MGMT_GROUP,
};

void kit_mgmt_init(void)
{
	mgmt_register_group(&group);
}
#else
void kit_mgmt_init(void)
{
}
#endif
