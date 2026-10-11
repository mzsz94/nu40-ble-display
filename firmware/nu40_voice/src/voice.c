/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "voice.h"
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_ALIAS(dmic0), okay), "Enable verified KIT PDM pin profile");
static const struct device *const microphone = DEVICE_DT_GET(DT_ALIAS(dmic0));
K_MEM_SLAB_DEFINE_STATIC(pcm_slab, VOICE_SAMPLES * sizeof(int16_t), 8, 4);
K_SEM_DEFINE(capture_start, 0, 1);
static struct k_spinlock state_lock;
static struct {
 struct bt_conn *peer;
 const struct bt_gatt_attr *audio_attr;
 uint32_t session;
 bool wanted, busy, subscribed, ready, ota, alarm;
} state;
struct audio_packet { uint8_t bytes[VOICE_FRAME_SIZE]; };
K_MSGQ_DEFINE(audio_queue, sizeof(struct audio_packet), 8, 4);

static void halt_capture(void) {
 k_spinlock_key_t key = k_spin_lock(&state_lock);
 state.wanted = false;
 k_spin_unlock(&state_lock, key);
}
void voice_ota_pause(bool pause) {
 k_spinlock_key_t key = k_spin_lock(&state_lock);
 state.ota = pause;
 if (pause) state.wanted = false;
 k_spin_unlock(&state_lock, key);
}
void voice_alarm_pause(bool pause) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 state.alarm=pause;
 if(pause)state.wanted=false;
 k_spin_unlock(&state_lock,key);
}
static bool still_wanted(struct bt_conn *conn, uint32_t session) {
 k_spinlock_key_t key = k_spin_lock(&state_lock);
 bool wanted = state.wanted && state.subscribed && state.peer == conn && state.session == session;
 k_spin_unlock(&state_lock, key);
 return wanted;
}
/* The system workqueue context makes GATT buffer allocation nonblocking.
 * Congestion stops the recording; missing frames are never hidden.
 */
static void send_audio(struct k_work *work) {
 struct audio_packet packet;
 ARG_UNUSED(work);
 while (k_msgq_get(&audio_queue, &packet, K_NO_WAIT) == 0) {
  struct bt_conn *conn = NULL;
  k_spinlock_key_t key = k_spin_lock(&state_lock);
  if (state.wanted && state.subscribed && state.peer && state.session == voice_u32(packet.bytes + 4)) conn = bt_conn_ref(state.peer);
  k_spin_unlock(&state_lock, key);
  if (!conn) continue;
  int ret = bt_gatt_get_mtu(conn) >= VOICE_MIN_MTU ? bt_gatt_notify(conn, state.audio_attr, packet.bytes, sizeof(packet.bytes)) : -EMSGSIZE;
  bt_conn_unref(conn);
  if (ret) { halt_capture(); k_msgq_purge(&audio_queue); printk("VOICE|STOP reason=notify error=%d\n", ret); return; }
 }
}
K_WORK_DEFINE(audio_work, send_audio);
static void mtu_done(struct bt_conn *conn,uint8_t error,struct bt_gatt_exchange_params *params) {
 ARG_UNUSED(params);printk("VOICE|MTU value=%u error=%u\n",bt_gatt_get_mtu(conn),error);
}
static struct bt_gatt_exchange_params mtu_params = {.func=mtu_done};
void voice_init(const struct bt_gatt_attr *audio_attr) {
 state.audio_attr=audio_attr; state.ready=device_is_ready(microphone);
 printk("VOICE|BOOT ready=%u source=NU40_PDM rate=16000 frame=176\n",state.ready);
}
void voice_connected(struct bt_conn *conn) {
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 state.peer=bt_conn_ref(conn);state.wanted=false;state.subscribed=false;
 k_spin_unlock(&state_lock,key);
 int ret=bt_gatt_exchange_mtu(conn,&mtu_params);
 printk("VOICE|MTU requested=247 result=%d\n",ret);
}
void voice_disconnected(struct bt_conn *conn) {
 struct bt_conn *release=NULL;
 k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(state.peer==conn){release=state.peer;state.peer=NULL;state.wanted=false;state.subscribed=false;}
 k_spin_unlock(&state_lock,key);
 if(release)bt_conn_unref(release);
 k_msgq_purge(&audio_queue);
}
void voice_subscription(const struct bt_gatt_attr *attr,uint16_t value) {
 ARG_UNUSED(attr);k_spinlock_key_t key=k_spin_lock(&state_lock);
 state.subscribed=value==BT_GATT_CCC_NOTIFY;
 if(!state.subscribed)state.wanted=false;
 k_spin_unlock(&state_lock,key);
}
ssize_t voice_control(struct bt_conn *conn,const struct bt_gatt_attr *attr,const void *data,uint16_t length,uint16_t offset,uint8_t flags) {
 bool start;uint32_t session;ARG_UNUSED(attr);
 if(offset)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
 if(flags&(BT_GATT_WRITE_FLAG_PREPARE|BT_GATT_WRITE_FLAG_CMD))return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
 if(!voice_parse_control(data,length,&start,&session))return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
 if(start&&bt_gatt_get_mtu(conn)<VOICE_MIN_MTU)return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
 uint8_t error=0;k_spinlock_key_t key=k_spin_lock(&state_lock);
 if(state.peer!=conn||!state.ready||state.ota||state.alarm)error=BT_ATT_ERR_UNLIKELY;
 else if(start&&!state.subscribed)error=BT_ATT_ERR_CCC_IMPROPER_CONF;
 else if(start&&state.busy)error=BT_ATT_ERR_INSUFFICIENT_RESOURCES;
 else if(!start&&state.session!=session)error=BT_ATT_ERR_VALUE_NOT_ALLOWED;
 else if(start){state.session=session;state.wanted=true;state.busy=true;}
 else state.wanted=false;
 k_spin_unlock(&state_lock,key);
 if(error)return BT_GATT_ERR(error);
 if(start)k_sem_give(&capture_start);
 return length;
}
static void capture_thread(void *a,void *b,void *c) {
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
 struct pcm_stream_cfg stream={.pcm_rate=VOICE_SAMPLE_RATE,.pcm_width=16,.block_size=VOICE_SAMPLES*sizeof(int16_t),.mem_slab=&pcm_slab};
 struct dmic_cfg cfg={.io={.min_pdm_clk_freq=1000000,.max_pdm_clk_freq=3500000,.min_pdm_clk_dc=40,.max_pdm_clk_dc=60},.streams=&stream,.channel={.req_num_streams=1,.req_num_chan=1}};
 cfg.channel.req_chan_map_lo=dmic_build_channel_map(0,0,IS_ENABLED(CONFIG_WALLABY_PDM_RIGHT_CHANNEL)?PDM_CHAN_RIGHT:PDM_CHAN_LEFT);
 while(true){
  k_sem_take(&capture_start,K_FOREVER);
  k_spinlock_key_t key=k_spin_lock(&state_lock);
  struct bt_conn *conn=state.peer?bt_conn_ref(state.peer):NULL;
  uint32_t session=state.session;k_spin_unlock(&state_lock,key);
  int ret=-ENOTCONN;uint32_t sequence=0;bool started=false;
  k_msgq_purge(&audio_queue);
  if(conn&&still_wanted(conn,session)){
   ret=dmic_configure(microphone,&cfg);
   if(!ret)ret=dmic_trigger(microphone,DMIC_TRIGGER_START);
   started=ret==0;
  }
  printk("VOICE|START session=%u result=%d\n",session,ret);
  while(started&&still_wanted(conn,session)&&sequence<VOICE_MAX_FRAMES){
   void *buffer=NULL;size_t size=0;struct audio_packet packet;
   ret=dmic_read(microphone,0,&buffer,&size,100);if(ret)break;
   bool valid=size==VOICE_SAMPLES*sizeof(int16_t)&&voice_encode(buffer,VOICE_SAMPLES,session,sequence,packet.bytes,sizeof(packet.bytes));
   k_mem_slab_free(&pcm_slab,buffer);
   if(!valid){ret=-EMSGSIZE;break;}
   if(!still_wanted(conn,session))break;
   ret=k_msgq_put(&audio_queue,&packet,K_NO_WAIT);if(ret)break;
   k_work_submit(&audio_work);sequence++;
  }
  halt_capture();
  if(started){int stopped=dmic_trigger(microphone,DMIC_TRIGGER_STOP);if(!ret)ret=stopped;}
  /* nrfx STOP is asynchronous and completed buffers remain in its RX queue.
   * Drain and wait boundedly, preventing stale samples in a new session. */
  int64_t deadline=k_uptime_get()+200;
  while(started&&k_mem_slab_num_free_get(&pcm_slab)<8&&k_uptime_get()<deadline){
   void *left=NULL;size_t remaining=0;
   if(dmic_read(microphone,0,&left,&remaining,20)==0)k_mem_slab_free(&pcm_slab,left);
  }
  k_msgq_purge(&audio_queue);if(conn)bt_conn_unref(conn);
  key=k_spin_lock(&state_lock);
  if(k_mem_slab_num_free_get(&pcm_slab)!=8){state.ready=false;ret=-EIO;}
  state.busy=false;k_spin_unlock(&state_lock,key);
  printk("VOICE|STOP session=%u frames=%u result=%d\n",session,sequence,ret);
 }
}
K_THREAD_DEFINE(voice_capture,3072,capture_thread,NULL,NULL,NULL,6,0,0);
