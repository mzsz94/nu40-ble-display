/* SPDX-License-Identifier: Apache-2.0 */
/* BTN4 simulates a detection event. It does not measure or identify explosives. */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include "alarm.h"
#include "voice.h"
#include "alarm_button.h"

#define TONE_SAMPLES 320U
#define TONE_BYTES (TONE_SAMPLES * 2U * sizeof(int16_t))
#define TONE_FRAMES 500U /* 10 seconds, latch remains until clear. */
static const struct device *const speaker = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct gpio_dt_spec amplifier = GPIO_DT_SPEC_GET(DT_NODELABEL(kit_amp),gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(wallaby_alarm_button),gpios);
K_MEM_SLAB_DEFINE_STATIC(tone_slab,TONE_BYTES,4,4);
K_SEM_DEFINE(alarm_ready,0,1);

static struct k_spinlock link_lock;
static struct {
 struct bt_conn *peer;
 const struct bt_gatt_attr *ack_attr;
 uint32_t disconnects;
 bool subscribed,resend;
} link;
struct alarm_request { struct bt_conn *conn; uint32_t sequence; bool on; };
struct alarm_ack { struct bt_conn *conn; uint8_t bytes[8]; };
K_MSGQ_DEFINE(requests,sizeof(struct alarm_request),2,4);
K_MSGQ_DEFINE(acks,sizeof(struct alarm_ack),4,4);
static bool latched,tone_running;
static atomic_t visible_alarm;

bool alarm_is_active(void)
{
 return atomic_get(&visible_alarm) != 0;
}
static uint8_t last_source,last_status;
static uint32_t last_sequence;
static uint32_t tone_frame;

static void send_acks(struct k_work *work)
{
 struct alarm_ack ack;
 ARG_UNUSED(work);
 while(k_msgq_get(&acks,&ack,K_NO_WAIT)==0){
  int ret=bt_gatt_notify(ack.conn,link.ack_attr,ack.bytes,sizeof(ack.bytes));
  bt_conn_unref(ack.conn);
  if(ret)printk("ALARM|notify_error=%d\n",ret);
 }
}
K_WORK_DEFINE(ack_work,send_acks);

static void acknowledge(uint32_t sequence,uint8_t source,uint8_t status)
{
 struct alarm_ack ack={.bytes={1,latched,0,0,0,0,status,source}};
 k_spinlock_key_t key=k_spin_lock(&link_lock);
 ack.conn=link.peer&&link.subscribed?bt_conn_ref(link.peer):NULL;
 k_spin_unlock(&link_lock,key);
 if(!ack.conn)return;
 voice_put32(ack.bytes+2,sequence);
 if(k_msgq_put(&acks,&ack,K_NO_WAIT)!=0){bt_conn_unref(ack.conn);return;}
 k_work_submit(&ack_work);
}

static int tone_block(void)
{
 static const int16_t sine[16]={0,574,1061,1386,1500,1386,1061,574,0,-574,-1061,-1386,-1500,-1386,-1061,-574};
 void *memory=NULL;
 int ret=k_mem_slab_alloc(&tone_slab,&memory,K_MSEC(30));
 if(ret)return ret;
 int16_t *pcm=memory;
 bool beep=(tone_frame%20)<10; /* 200 ms on / 200 ms off. */
 for(size_t i=0;i<TONE_SAMPLES;i++)pcm[i*2]=pcm[i*2+1]=beep?sine[i%16]:0;
 ret=i2s_write(speaker,memory,TONE_BYTES);
 if(ret)k_mem_slab_free(&tone_slab,memory);
 else tone_frame++;
 return ret;
}

static void silence(void)
{
 /* Shut the amplifier first, including driver-error and disconnect paths. */
 (void)gpio_pin_set_dt(&amplifier,0);
 if(tone_running)(void)i2s_trigger(speaker,I2S_DIR_TX,I2S_TRIGGER_DROP);
 tone_running=false;
}

static int begin_tone(void)
{
 struct i2s_config config={.word_size=16,.channels=2,.format=I2S_FMT_DATA_FORMAT_I2S,
  .options=I2S_OPT_BIT_CLK_CONTROLLER|I2S_OPT_FRAME_CLK_CONTROLLER,.frame_clk_freq=16000,
  .mem_slab=&tone_slab,.block_size=TONE_BYTES,.timeout=30};
 if(!device_is_ready(speaker)||!gpio_is_ready_dt(&amplifier))return -ENODEV;
 silence();
 int ret=i2s_configure(speaker,I2S_DIR_TX,&config);
 tone_frame=0;
 if(!ret)ret=tone_block();
 if(!ret)ret=tone_block();
 if(!ret)ret=i2s_trigger(speaker,I2S_DIR_TX,I2S_TRIGGER_START);
 if(!ret){tone_running=true;ret=gpio_pin_set_dt(&amplifier,1);}
 if(ret){(void)i2s_trigger(speaker,I2S_DIR_TX,I2S_TRIGGER_DROP);silence();}
 return ret;
}

static void apply_alarm(bool on,uint32_t sequence,uint8_t source)
{
 int ret=0;
 latched=on;last_source=source;last_sequence=sequence;
 atomic_set(&visible_alarm,on);
 if(on){voice_alarm_pause(true);ret=begin_tone();}
 else{silence();voice_alarm_pause(false);}
 last_status=ret?2:0;
 acknowledge(sequence,source,last_status);
 printk("ALARM|active=%u source=%u output_result=%d simulated=1\n",latched,source,ret);
}

void alarm_init(const struct bt_gatt_attr *ack_attr)
{
 link.ack_attr=ack_attr;
 k_sem_give(&alarm_ready);
}

void alarm_connected(struct bt_conn *conn)
{
 k_spinlock_key_t key=k_spin_lock(&link_lock);
 link.peer=bt_conn_ref(conn);link.subscribed=false;
 k_spin_unlock(&link_lock,key);
}

void alarm_disconnected(struct bt_conn *conn)
{
 struct bt_conn *release=NULL;
 k_spinlock_key_t key=k_spin_lock(&link_lock);
 if(link.peer==conn){release=link.peer;link.peer=NULL;link.subscribed=false;link.disconnects++;}
 k_spin_unlock(&link_lock,key);
 if(release)bt_conn_unref(release);
}

void alarm_subscription(const struct bt_gatt_attr *attr,uint16_t value)
{
 ARG_UNUSED(attr);
 k_spinlock_key_t key=k_spin_lock(&link_lock);
 link.subscribed=value==BT_GATT_CCC_NOTIFY;
 link.resend=link.subscribed;
 k_spin_unlock(&link_lock,key);
}

ssize_t alarm_control(struct bt_conn *conn,const struct bt_gatt_attr *attr,const void *data,uint16_t length,uint16_t offset,uint8_t flags)
{
 struct alarm_request request;
 ARG_UNUSED(attr);
 if(offset)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
 if(flags&(BT_GATT_WRITE_FLAG_PREPARE|BT_GATT_WRITE_FLAG_CMD))return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
 if(!voice_parse_control(data,length,&request.on,&request.sequence))return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
 if(!bt_gatt_is_subscribed(conn,link.ack_attr,BT_GATT_CCC_NOTIFY))return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
 request.conn=bt_conn_ref(conn);
 if(k_msgq_put(&requests,&request,K_NO_WAIT)!=0){bt_conn_unref(request.conn);return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);}
 return length;
}

static void alarm_thread(void *a,void *b,void *c)
{
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
 k_sem_take(&alarm_ready,K_FOREVER);
 int button_result=gpio_is_ready_dt(&button)?gpio_pin_configure_dt(&button,GPIO_INPUT):-ENODEV;
 struct alarm_button_state button_state={0};
 uint32_t disconnects=0;
 printk("ALARM|BTN4=P1.05 init=%d source=simulation\n",button_result);
 while(true){
  struct alarm_request request;
  if(k_msgq_get(&requests,&request,tone_running?K_NO_WAIT:K_MSEC(10))==0){
   k_spinlock_key_t key=k_spin_lock(&link_lock);
   bool current=link.peer==request.conn;
   k_spin_unlock(&link_lock,key);
   if(current)apply_alarm(request.on,request.sequence,0);
   bt_conn_unref(request.conn);
  }
  if(button_result==0){
   int reading=gpio_pin_get_dt(&button);
   if(reading>=0){
    if(alarm_button_press(&button_state,reading!=0,k_uptime_get_32())&&!latched)
     apply_alarm(true,0,1);
   }
  }
  k_spinlock_key_t key=k_spin_lock(&link_lock);
  uint32_t current_disconnects=link.disconnects;
  bool resend=link.resend;link.resend=false;
  k_spin_unlock(&link_lock,key);
  if(current_disconnects!=disconnects){silence();disconnects=current_disconnects;}
  if(resend&&latched)acknowledge(last_sequence,last_source,last_status);
  if(tone_running){
   int ret=tone_frame<TONE_FRAMES?tone_block():0;
   if(ret||tone_frame>=TONE_FRAMES){
    silence();
    if(ret){last_status=2;acknowledge(last_sequence,last_source,last_status);}
    /* Keep the latch and microphone gate after the finite warning pattern. */
   }
  }
 }
}
K_THREAD_DEFINE(alarm_worker,2048,alarm_thread,NULL,NULL,NULL,5,0,0);
