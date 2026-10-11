/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "voice.h"
#include "recording.h"
#include "alarm_button.h"

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_ALIAS(dmic0), okay), "Enable verified KIT PDM pin profile");
static const struct device *const microphone = DEVICE_DT_GET(DT_ALIAS(dmic0));
static const struct gpio_dt_spec record_button = GPIO_DT_SPEC_GET(DT_ALIAS(wallaby_record_button), gpios);
static const struct gpio_dt_spec send_button = GPIO_DT_SPEC_GET(DT_ALIAS(wallaby_send_button), gpios);
K_MEM_SLAB_DEFINE_STATIC(pcm_slab, VOICE_SAMPLES * sizeof(int16_t), 8, 4);
K_SEM_DEFINE(voice_ready, 0, 1);
K_SEM_DEFINE(voice_wake, 0, 1);
static struct k_spinlock state_lock;
static struct {
 struct bt_conn *peer;
 const struct bt_gatt_attr *audio_attr;
 struct recording_transport transport;
 bool subscribed, ready, ota, alarm;
 uint32_t display_revision;
 enum voice_status display_status;
 char display[85];
} state;
static struct recording clip; /* Owned exclusively by capture_thread. */
struct audio_packet { uint8_t bytes[VOICE_FRAME_SIZE]; uint32_t generation; };
K_MSGQ_DEFINE(audio_queue, sizeof(struct audio_packet), 8, 4);

static void display_locked(enum voice_status status,const char *text) {
 state.display_status=status;
 strncpy(state.display,text,sizeof(state.display)-1);
 state.display[sizeof(state.display)-1]=0;state.display_revision++;
}
static void display_status(enum voice_status status,const char *text) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);display_locked(status,text);k_spin_unlock(&state_lock,key);
}
bool voice_display_status(uint32_t *revision,enum voice_status *status,char *text,size_t capacity) {
 if(!revision||!status||!text||!capacity)return false;
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 bool changed=*revision!=state.display_revision;
 if(changed){strncpy(text,state.display,capacity-1);text[capacity-1]=0;*revision=state.display_revision;*status=state.display_status;}
 k_spin_unlock(&state_lock,key);return changed;
}
static void cancel_locked(const char *message) {
 (void)recording_stop(&state.transport,state.transport.session);
 display_locked(VOICE_OFF,message);
}
static void fail_session(uint32_t generation,const char *message) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(state.transport.generation==generation){cancel_locked(message);state.display_status=VOICE_ERROR;}
 k_spin_unlock(&state_lock,key);k_sem_give(&voice_wake);
}
void voice_ota_pause(bool pause) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);state.ota=pause;
 if(pause)cancel_locked("MIC OFF - OTA\nRecording discarded");
 k_spin_unlock(&state_lock,key);k_sem_give(&voice_wake);
}
void voice_alarm_pause(bool pause) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);bool was_active=state.alarm;state.alarm=pause;
 if(pause)cancel_locked("ALERT - MIC OFF\nRecording discarded");
 else if(was_active&&!state.ota)display_locked(VOICE_OFF,"MIC OFF\nAlarm cleared\nArm in HostPC");
 k_spin_unlock(&state_lock,key);k_sem_give(&voice_wake);
}
static bool current_session(uint32_t session,uint32_t generation) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 bool current=recording_session_matches(&state.transport,session,generation)&&state.peer&&state.subscribed&&!state.alarm&&!state.ota;
 k_spin_unlock(&state_lock,key);return current;
}
static void display_session_status(uint32_t session,uint32_t generation,
                                   enum voice_status status,const char *text) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(recording_session_matches(&state.transport,session,generation))display_locked(status,text);
 k_spin_unlock(&state_lock,key);
}
/* System-workqueue notification allocation is nonblocking. A failed frame
 * cancels the session: the host must never silently bridge a missing packet. */
static void send_audio(struct k_work *work) {
 struct audio_packet packet;ARG_UNUSED(work);
 while(k_msgq_get(&audio_queue,&packet,K_NO_WAIT)==0){
  struct bt_conn *conn=NULL;
  k_spinlock_key_t key=k_spin_lock(&state_lock);
  if(recording_session_matches(&state.transport,voice_u32(packet.bytes+4),packet.generation)&&state.peer&&state.subscribed&&!state.alarm&&!state.ota)
   conn=bt_conn_ref(state.peer);
  k_spin_unlock(&state_lock,key);
  if(!conn)continue;
  int ret=bt_gatt_get_mtu(conn)>=VOICE_MIN_MTU?bt_gatt_notify(conn,state.audio_attr,packet.bytes,VOICE_FRAME_SIZE):-EMSGSIZE;
  bt_conn_unref(conn);
  if(ret){fail_session(packet.generation,"MIC SEND ERROR\nRe-arm in HostPC");printk("VOICE|STOP reason=notify error=%d\n",ret);return;}
 }
}
K_WORK_DEFINE(audio_work,send_audio);
static bool enqueue(uint8_t bytes[VOICE_FRAME_SIZE],uint32_t session,uint32_t generation) {
 struct audio_packet packet={.generation=generation};
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(!recording_session_matches(&state.transport,session,generation)||state.transport.sequence>=RECORDING_HOST_RENEW){k_spin_unlock(&state_lock,key);return false;}
 voice_put32(bytes+4,session);voice_put32(bytes+8,state.transport.sequence++);
 memcpy(packet.bytes,bytes,VOICE_FRAME_SIZE);
 k_spin_unlock(&state_lock,key);
 if(k_msgq_put(&audio_queue,&packet,K_NO_WAIT)){fail_session(generation,"MIC QUEUE FULL\nRe-arm in HostPC");return false;}
 if(k_work_submit(&audio_work)<0){fail_session(generation,"MIC WORK ERROR\nRe-arm in HostPC");return false;}
 return true;
}
static bool send_silence(uint32_t session,uint32_t generation) {
 /* All-zero independent IMA block decodes to exactly 320 zero samples. */
 uint8_t bytes[VOICE_FRAME_SIZE]={ 'W','A',1,0 };
 return enqueue(bytes,session,generation);
}
static void mtu_done(struct bt_conn *conn,uint8_t error,struct bt_gatt_exchange_params *params) {
 ARG_UNUSED(params);printk("VOICE|MTU value=%u error=%u\n",bt_gatt_get_mtu(conn),error);
}
static struct bt_gatt_exchange_params mtu_params={.func=mtu_done};
void voice_init(const struct bt_gatt_attr *audio_attr) {
 state.audio_attr=audio_attr;state.ready=device_is_ready(microphone);
 display_status(VOICE_OFF,"MIC OFF\nArm in HostPC\nBTN2 REC / BTN3 SEND");
 printk("VOICE|BOOT ready=%u source=NU40_PDM rate=16000 frame=176 buttons=2,3\n",state.ready);
 k_sem_give(&voice_ready);
}
void voice_connected(struct bt_conn *conn) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 state.peer=bt_conn_ref(conn);state.subscribed=false;cancel_locked("MIC OFF\nArm in HostPC");
 k_spin_unlock(&state_lock,key);
 int ret=bt_gatt_exchange_mtu(conn,&mtu_params);printk("VOICE|MTU requested=247 result=%d\n",ret);
}
void voice_disconnected(struct bt_conn *conn) {
 struct bt_conn *release=NULL;k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(state.peer==conn){release=state.peer;state.peer=NULL;state.subscribed=false;cancel_locked("MIC OFF - NO LINK\nRecording discarded");}
 k_spin_unlock(&state_lock,key);if(release)bt_conn_unref(release);k_sem_give(&voice_wake);
}
void voice_subscription(const struct bt_gatt_attr *attr,uint16_t value) {
 ARG_UNUSED(attr);k_spinlock_key_t key=k_spin_lock(&state_lock);
 state.subscribed=value==BT_GATT_CCC_NOTIFY;
 if(!state.subscribed)cancel_locked("MIC OFF - NO AUDIO\nRecording discarded");
 k_spin_unlock(&state_lock,key);k_sem_give(&voice_wake);
}
ssize_t voice_control(struct bt_conn *conn,const struct bt_gatt_attr *attr,const void *data,uint16_t length,uint16_t offset,uint8_t flags) {
 bool start;uint32_t session;ARG_UNUSED(attr);
 if(offset)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
 if(flags&(BT_GATT_WRITE_FLAG_PREPARE|BT_GATT_WRITE_FLAG_CMD))return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
 if(!voice_parse_control(data,length,&start,&session))return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
 if(start&&bt_gatt_get_mtu(conn)<VOICE_MIN_MTU)return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
 uint8_t error=0;k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(state.peer!=conn)error=BT_ATT_ERR_UNLIKELY;
 else if(start&&(!state.ready||state.ota||state.alarm))error=BT_ATT_ERR_UNLIKELY;
 else if(start&&!state.subscribed)error=BT_ATT_ERR_CCC_IMPROPER_CONF;
 else if(start){recording_arm(&state.transport,session);display_locked(VOICE_ARMED,"MIC READY\nBTN2: RECORD\nBTN3: SEND");}
 else if(!recording_stop(&state.transport,session))error=BT_ATT_ERR_VALUE_NOT_ALLOWED;
 else display_locked(VOICE_OFF,"MIC OFF\nRecording discarded\nArm in HostPC");
 k_spin_unlock(&state_lock,key);
 if(error)return BT_GATT_ERR(error);
 k_sem_give(&voice_wake);return length;
}
static int stop_microphone(bool *running) {
 if(!*running)return 0;
 int ret=dmic_trigger(microphone,DMIC_TRIGGER_STOP);*running=false;
 int64_t deadline=k_uptime_get()+200;
 while(k_mem_slab_num_free_get(&pcm_slab)<8&&k_uptime_get()<deadline){
  void *left=NULL;size_t remaining=0;
  if(dmic_read(microphone,0,&left,&remaining,20)==0)k_mem_slab_free(&pcm_slab,left);
 }
 if(k_mem_slab_num_free_get(&pcm_slab)!=8){
  k_spinlock_key_t key=k_spin_lock(&state_lock);state.ready=false;k_spin_unlock(&state_lock,key);return -EIO;
 }
 return ret;
}
static void capture_thread(void *a,void *b,void *c) {
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);k_sem_take(&voice_ready,K_FOREVER);
 struct pcm_stream_cfg stream={.pcm_rate=VOICE_SAMPLE_RATE,.pcm_width=16,.block_size=VOICE_SAMPLES*sizeof(int16_t),.mem_slab=&pcm_slab};
 struct dmic_cfg cfg={.io={.min_pdm_clk_freq=1000000,.max_pdm_clk_freq=3500000,.min_pdm_clk_dc=40,.max_pdm_clk_dc=60},.streams=&stream,.channel={.req_num_streams=1,.req_num_chan=1}};
 cfg.channel.req_chan_map_lo=dmic_build_channel_map(0,0,IS_ENABLED(CONFIG_WALLABY_PDM_RIGHT_CHANNEL)?PDM_CHAN_RIGHT:PDM_CHAN_LEFT);
 int button_error=!gpio_is_ready_dt(&record_button)||!gpio_is_ready_dt(&send_button)?-ENODEV:gpio_pin_configure_dt(&record_button,GPIO_INPUT);
 if(!button_error)button_error=gpio_pin_configure_dt(&send_button,GPIO_INPUT);
 if(button_error){k_spinlock_key_t key=k_spin_lock(&state_lock);state.ready=false;k_spin_unlock(&state_lock,key);display_status(VOICE_ERROR,"MIC BUTTON ERROR\nCheck KIT wiring");}
 printk("VOICE|BTN2=P1.07 BTN3=P1.06 init=%d\n",button_error);
 struct alarm_button_state btn2={0},btn3={0};
 bool running=false,active=false;uint32_t generation=0,session=0,arm_started=0;
 int64_t last_keepalive=0,next_send=0;unsigned tail=0;
 while(true){
  k_spinlock_key_t key=k_spin_lock(&state_lock);struct recording_transport transport=state.transport;
  bool usable=transport.armed&&state.peer&&state.subscribed&&state.ready&&!state.ota&&!state.alarm;
  k_spin_unlock(&state_lock,key);
  if(transport.generation!=generation||(!usable&&active)){
   int stopped=stop_microphone(&running);recording_cancel(&clip);tail=0;
   generation=transport.generation;session=transport.session;active=usable;
   arm_started=k_uptime_get_32();last_keepalive=k_uptime_get()-1000;
   if(stopped){fail_session(generation,"MIC STOP ERROR\nReset required");active=false;}
  }
  uint32_t now=k_uptime_get_32();bool press2=false,press3=false;
  if(!button_error){
   int p2=gpio_pin_get_dt(&record_button),p3=gpio_pin_get_dt(&send_button);
   if(p2<0||p3<0){fail_session(generation,"MIC BUTTON ERROR\nCheck KIT wiring");active=false;}
   else{press2=alarm_button_press(&btn2,p2!=0,now);press3=alarm_button_press(&btn3,p3!=0,now);}
  }
  if(!active||!current_session(session,generation)){k_sem_take(&voice_wake,K_MSEC(10));continue;}
  uint32_t elapsed=now-arm_started;
  if(elapsed>=RECORDING_ARM_MS){fail_session(generation,"MIC ARM EXPIRED\nRe-arm in HostPC");continue;}
  if(press2){
   if(clip.state==RECORDING_SEND)display_session_status(session,generation,VOICE_SENDING,"MIC SENDING\nWait for response");
   else if(!recording_start_budget(transport.sequence,elapsed))fail_session(generation,"MIC RE-ARM REQUIRED\nStop/start in HostPC");
   else{
    int ret=stop_microphone(&running);recording_start(&clip);tail=0;
    if(!ret)ret=dmic_configure(microphone,&cfg);
    if(!ret)ret=dmic_trigger(microphone,DMIC_TRIGGER_START);
    running=ret==0;
    if(ret)fail_session(generation,"MIC START ERROR\nRe-arm in HostPC");
    else{display_session_status(session,generation,VOICE_RECORDING,"MIC REC (MAX 10 SEC)\nBTN2: RESTART\nBTN3: STOP + SEND");printk("VOICE|RECORD session=%u\n",session);}
   }
  }
  if(press3&&(clip.state==RECORDING_CAPTURE||clip.state==RECORDING_READY)){
   int ret=stop_microphone(&running);
   if(ret)fail_session(generation,"MIC STOP ERROR\nReset required");
   else if(!clip.count){recording_cancel(&clip);display_session_status(session,generation,VOICE_ERROR,"MIC EMPTY\nBTN2: RECORD AGAIN");}
   else if(!recording_send_budget(transport.sequence,clip.count,elapsed))fail_session(generation,"MIC RE-ARM REQUIRED\nRecording discarded");
   else if(recording_submit(&clip)){tail=RECORDING_TAIL_FRAMES;next_send=k_uptime_get();display_session_status(session,generation,VOICE_SENDING,"MIC SENDING\nWait for response");printk("VOICE|SEND session=%u frames=%u\n",session,clip.count);}
  }
  if(!current_session(session,generation))continue;
  if(running&&clip.state==RECORDING_CAPTURE){
   void *buffer=NULL;size_t size=0;int ret=dmic_read(microphone,0,&buffer,&size,100);
   if(!ret){bool saved=current_session(session,generation)&&size==VOICE_SAMPLES*sizeof(int16_t)&&recording_append(&clip,buffer,VOICE_SAMPLES);k_mem_slab_free(&pcm_slab,buffer);if(!saved)ret=-EIO;}
   if(ret){fail_session(generation,"MIC READ ERROR\nRecording discarded");continue;}
   if(clip.state==RECORDING_READY){
    ret=stop_microphone(&running);
    if(ret)fail_session(generation,"MIC STOP ERROR\nReset required");
    else display_session_status(session,generation,VOICE_READY,"MIC REC LIMIT 10 SEC\nBTN3: SEND\nBTN2: RECORD AGAIN");
   }
  }
  int64_t tick=k_uptime_get();
  if(clip.state==RECORDING_SEND){
   if(tick>=next_send){
    bool sent=false;
    if(clip.cursor<clip.count){uint8_t bytes[VOICE_FRAME_SIZE];if(recording_next(&clip,session,0,bytes,sizeof(bytes)))sent=enqueue(bytes,session,generation);}
    else if(tail){sent=send_silence(session,generation);if(sent)tail--;}
    if(!sent){fail_session(generation,"MIC SEND CANCELLED\nRe-arm in HostPC");continue;}
    next_send=tick+20;last_keepalive=tick;
    if(clip.cursor==clip.count&&!tail){recording_cancel(&clip);display_session_status(session,generation,VOICE_SENT,"MIC SENT\nWait for response\nBTN2: NEW RECORD");}
   }
  }else if(tick-last_keepalive>=1000){
   if(!send_silence(session,generation)){fail_session(generation,"MIC RE-ARM REQUIRED\nStop/start in HostPC");continue;}
   last_keepalive=tick;
  }
  if(!running)k_sem_take(&voice_wake,K_MSEC(10));
 }
}
K_THREAD_DEFINE(voice_capture,3072,capture_thread,NULL,NULL,NULL,6,0,0);
