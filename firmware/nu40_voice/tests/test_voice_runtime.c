/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
/* Include actual callbacks and queue worker, rather than a duplicate model. */
#include "../src/voice.c"

static struct bt_conn connection={.references=1,.mtu=247};
static struct bt_gatt_attr attribute;
static ssize_t control(bool start,uint32_t session) {
 uint8_t bytes[6]={1,start};voice_put32(bytes+2,session);
 return voice_control(&connection,&attribute,bytes,sizeof(bytes),0,0);
}
static void setup(void) {
 memset(&state,0,sizeof(state));recording_cancel(&clip);
 audio_queue.count=audio_queue.head=0;
 fake_notify_calls=0;fake_notify_error=0;fake_work_error=0;
 connection.references=1;connection.mtu=247;
 voice_init(&attribute);voice_connected(&connection);voice_subscription(&attribute,BT_GATT_CCC_NOTIFY);
}
static void teardown(void) { voice_disconnected(&connection);assert(connection.references==1); }

int main(void) {
 char display[85];
 setup();assert(!voice_display_override(display,sizeof(display)));
 assert(control(true,76)==6);assert(voice_display_override(display,sizeof(display)));
 assert(strstr(display,"MIC READY")&&strstr(display,"BTN2: RECORD"));
 char short_display[2];assert(voice_display_override(short_display,sizeof(short_display)));
 assert(short_display[0]=='M'&&short_display[1]==0);
 assert(!voice_display_override(NULL,sizeof(display)));
 assert(!voice_display_override(display,0));
 assert(control(false,76)==6);assert(!voice_display_override(display,sizeof(display)));
 assert(control(true,76)==6);voice_alarm_pause(true);
 assert(!voice_display_override(display,sizeof(display)));teardown();
 setup();assert(control(true,77)==6);assert(fake_dmic_starts==0);
 uint32_t old_generation=state.transport.generation;
 assert(send_silence(77,old_generation));assert(audio_queue.count==1);
 assert(control(false,77)==6);send_audio(&audio_work);
 assert(fake_notify_calls==0&&!state.transport.armed);
 assert(!send_silence(77,old_generation));teardown();

 /* Restart while old packets await the workqueue cannot replay those packets
  * under the new session, including when the same session ID is reused. */
 setup();assert(control(true,88)==6);old_generation=state.transport.generation;
 assert(send_silence(88,old_generation));assert(control(true,99)==6);
 assert(control(false,88)<0&&state.transport.armed);
 assert(send_silence(99,state.transport.generation));send_audio(&audio_work);
 assert(fake_notify_calls==1&&voice_u32(fake_notifications[0]+4)==99);
 assert(voice_u32(fake_notifications[0]+8)==0);
 assert(send_silence(99,state.transport.generation));assert(control(true,99)==6);
 assert(send_silence(99,state.transport.generation));send_audio(&audio_work);
 assert(fake_notify_calls==2&&voice_u32(fake_notifications[1]+8)==0);teardown();

 /* A notification allocation/transport failure cancels the whole generation.
  * No remaining voice or silence frame can bridge a gap and trigger VAD. */
 setup();assert(control(true,101)==6);old_generation=state.transport.generation;
 assert(send_silence(101,old_generation));assert(send_silence(101,old_generation));
 fake_notify_error=-ENOMEM;send_audio(&audio_work);
 assert(fake_notify_calls==1&&!state.transport.armed);
 fake_notify_error=0;send_audio(&audio_work);
 assert(fake_notify_calls==1&&!send_silence(101,old_generation));
 voice_alarm_pause(false);voice_ota_pause(false);assert(!state.transport.armed);
 assert(control(true,102)==6&&state.transport.armed);assert(fake_dmic_starts==0);teardown();

 /* Queue overflow and work-submission failure follow the same fail-closed
  * path; queued packets are invalidated before the worker can send them. */
 setup();assert(control(true,110)==6);old_generation=state.transport.generation;
 for(unsigned i=0;i<8;i++)assert(send_silence(110,old_generation));
 assert(!send_silence(110,old_generation)&&!state.transport.armed);
 send_audio(&audio_work);assert(fake_notify_calls==0);teardown();
 setup();assert(control(true,111)==6);fake_work_error=-EBUSY;
 assert(!send_silence(111,state.transport.generation)&&!state.transport.armed);
 send_audio(&audio_work);assert(fake_notify_calls==0);teardown();

 setup();assert(control(true,120)==6);assert(send_silence(120,state.transport.generation));
 voice_alarm_pause(true);send_audio(&audio_work);assert(fake_notify_calls==0);
 assert(control(true,121)<0);voice_alarm_pause(false);assert(!state.transport.armed);
 assert(control(true,121)==6);assert(send_silence(121,state.transport.generation));
 voice_ota_pause(true);send_audio(&audio_work);assert(fake_notify_calls==0);
 assert(control(true,122)<0);voice_ota_pause(false);assert(!state.transport.armed);
 assert(control(true,122)==6);assert(send_silence(122,state.transport.generation));
 voice_subscription(&attribute,0);send_audio(&audio_work);assert(fake_notify_calls==0);
 voice_subscription(&attribute,BT_GATT_CCC_NOTIFY);assert(!state.transport.armed);
 assert(control(true,123)==6);assert(send_silence(123,state.transport.generation));
 teardown();send_audio(&audio_work);assert(fake_notify_calls==0&&!state.transport.armed);
 return 0;
}
