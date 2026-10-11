/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/init.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include "voice.h"

static enum mgmt_cb_return image_event(uint32_t event, enum mgmt_cb_return previous,
 int32_t *rc, uint16_t *group, bool *abort_more, void *data, size_t size)
{
 ARG_UNUSED(previous); ARG_UNUSED(rc); ARG_UNUSED(group); ARG_UNUSED(abort_more);
 ARG_UNUSED(data); ARG_UNUSED(size);
 if (event == MGMT_EVT_OP_IMG_MGMT_DFU_STARTED) voice_ota_pause(true);
 if (event == MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED || event == MGMT_EVT_OP_IMG_MGMT_DFU_PENDING)
  voice_ota_pause(false);
 return MGMT_CB_OK;
}
static struct mgmt_callback callback = {.callback=image_event,
 .event_id=MGMT_EVT_OP_IMG_MGMT_DFU_STARTED | MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED | MGMT_EVT_OP_IMG_MGMT_DFU_PENDING};
static int register_ota_events(void)
{
 mgmt_callback_register(&callback);
 return 0;
}
/* Test images remain unconfirmed until the HostPC explicitly confirms them. */
SYS_INIT(register_ota_events, APPLICATION, 90);
