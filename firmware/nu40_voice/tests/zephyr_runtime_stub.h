/* SPDX-License-Identifier: Apache-2.0 */
/* Deterministic host harness for the production BLE callbacks/work item.
 * DMIC and GPIO are not exercised here; the capture thread is not started. */
#ifndef VOICE_ZEPHYR_RUNTIME_STUB_H
#define VOICE_ZEPHYR_RUNTIME_STUB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(c,m) _Static_assert(c,m)
#define DT_ALIAS(x) 0
#define DT_NODELABEL(x) 0
#define DT_NODE_HAS_STATUS(n,s) 1
#define IS_ENABLED(x) 0
#define K_NO_WAIT 0
#define K_FOREVER -1
#define K_MSEC(x) (x)
#define GPIO_INPUT 0
struct device { int unused; };
static const struct device fake_device;
#define DEVICE_DT_GET(x) (&fake_device)
static inline bool device_is_ready(const struct device *d) { ARG_UNUSED(d); return true; }
struct gpio_dt_spec { int unused; };
#define GPIO_DT_SPEC_GET(n,p) {0}
static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *s) { ARG_UNUSED(s); return true; }
static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *s,int f) { ARG_UNUSED(s); ARG_UNUSED(f); return 0; }
static inline int gpio_pin_get_dt(const struct gpio_dt_spec *s) { ARG_UNUSED(s); return 0; }
static inline int gpio_pin_set_dt(const struct gpio_dt_spec *s,int v) { ARG_UNUSED(s);ARG_UNUSED(v);return 0; }
typedef int atomic_t;
static inline int atomic_get(const atomic_t *a) { return *a; }
static inline void atomic_set(atomic_t *a,int v) { *a=v; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static inline int k_spin_lock(struct k_spinlock *s) { ARG_UNUSED(s); return 0; }
static inline void k_spin_unlock(struct k_spinlock *s,int k) { ARG_UNUSED(s); ARG_UNUSED(k); }
struct k_sem { int unused; };
#define K_SEM_DEFINE(n,i,m) static struct k_sem n
static inline void k_sem_give(struct k_sem *s) { ARG_UNUSED(s); }
static inline int k_sem_take(struct k_sem *s,int t) { ARG_UNUSED(s); ARG_UNUSED(t); return 0; }
struct k_mem_slab { int unused; };
#define K_MEM_SLAB_DEFINE_STATIC(n,s,c,a) static struct k_mem_slab n
static inline unsigned k_mem_slab_num_free_get(struct k_mem_slab *s) { ARG_UNUSED(s); return 8; }
static inline void k_mem_slab_free(struct k_mem_slab *s,void *p) { ARG_UNUSED(s); ARG_UNUSED(p); }
static inline int k_mem_slab_alloc(struct k_mem_slab *s,void **p,int t) {
 ARG_UNUSED(s);ARG_UNUSED(p);ARG_UNUSED(t);return -ENOMEM;
}
struct k_msgq { unsigned count,head,capacity; size_t size; unsigned char *data; };
#define K_MSGQ_DEFINE(n,s,c,a) static unsigned char n##_data[(s)*(c)]; static struct k_msgq n={0,0,c,s,n##_data}
static inline int k_msgq_put(struct k_msgq *q,const void *p,int t) {
 ARG_UNUSED(t);if(q->count==q->capacity)return -ENOMSG;
 memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,p,q->size);q->count++;return 0;
}
static inline int k_msgq_get(struct k_msgq *q,void *p,int t) {
 ARG_UNUSED(t);if(!q->count)return -ENOMSG;
 memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;return 0;
}
struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(n,h) static struct k_work n={h}
static int fake_work_error;
static inline int k_work_submit(struct k_work *w) { ARG_UNUSED(w);return fake_work_error; }
#define K_THREAD_DEFINE(...)
static inline int64_t k_uptime_get(void) { return 1000; }
static inline uint32_t k_uptime_get_32(void) { return 1000; }
static inline int printk(const char *fmt,...) { ARG_UNUSED(fmt);return 0; }
struct pcm_stream_cfg { unsigned pcm_rate,pcm_width,block_size; struct k_mem_slab *mem_slab; };
struct dmic_cfg {
 struct { unsigned min_pdm_clk_freq,max_pdm_clk_freq,min_pdm_clk_dc,max_pdm_clk_dc; } io;
 struct pcm_stream_cfg *streams;
 struct { unsigned req_num_streams,req_num_chan,req_chan_map_lo; } channel;
};
#define PDM_CHAN_RIGHT 1
#define PDM_CHAN_LEFT 0
#define DMIC_TRIGGER_STOP 0
#define DMIC_TRIGGER_START 1
static unsigned fake_dmic_starts;
static inline unsigned dmic_build_channel_map(unsigned a,unsigned b,unsigned c) { return a+b+c; }
static inline int dmic_trigger(const struct device *d,int op) { ARG_UNUSED(d);if(op==DMIC_TRIGGER_START)fake_dmic_starts++;return 0; }
static inline int dmic_configure(const struct device *d,struct dmic_cfg *c) { ARG_UNUSED(d);ARG_UNUSED(c);return 0; }
static inline int dmic_read(const struct device *d,unsigned stream,void **b,size_t *size,int timeout) {
 ARG_UNUSED(d);ARG_UNUSED(stream);ARG_UNUSED(b);ARG_UNUSED(size);ARG_UNUSED(timeout);return -EIO;
}
struct bt_conn { int references; unsigned mtu; bool notify_enabled; };
struct bt_gatt_attr { int unused; };
struct bt_gatt_exchange_params { void (*func)(struct bt_conn *,uint8_t,struct bt_gatt_exchange_params *); };
#define BT_GATT_CCC_NOTIFY 1
#define BT_GATT_WRITE_FLAG_PREPARE 1
#define BT_GATT_WRITE_FLAG_CMD 2
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_NOT_SUPPORTED 6
#define BT_ATT_ERR_VALUE_NOT_ALLOWED 19
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_ATT_ERR_UNLIKELY 14
#define BT_ATT_ERR_CCC_IMPROPER_CONF 253
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES 17
#define BT_GATT_ERR(x) (-(x))
static unsigned fake_notify_calls;
static int fake_notify_error;
static uint8_t fake_notifications[32][176];
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { c->references++;return c; }
static inline void bt_conn_unref(struct bt_conn *c) { c->references--; }
static inline unsigned bt_gatt_get_mtu(struct bt_conn *c) { return c->mtu; }
static inline bool bt_gatt_is_subscribed(struct bt_conn *c,const struct bt_gatt_attr *a,int v) {
 ARG_UNUSED(a);return c->notify_enabled&&v==BT_GATT_CCC_NOTIFY;
}
static inline int bt_gatt_exchange_mtu(struct bt_conn *c,struct bt_gatt_exchange_params *p) { ARG_UNUSED(c);ARG_UNUSED(p);return 0; }
static inline int bt_gatt_notify(struct bt_conn *c,const struct bt_gatt_attr *a,const void *p,uint16_t n) {
 ARG_UNUSED(c);ARG_UNUSED(a);if((n!=176&&n!=8)||fake_notify_calls>=32)return -EINVAL;
 memcpy(fake_notifications[fake_notify_calls++],p,n);return fake_notify_error;
}
#define I2S_DIR_TX 0
#define I2S_TRIGGER_DROP 0
#define I2S_TRIGGER_START 1
#define I2S_FMT_DATA_FORMAT_I2S 0
#define I2S_OPT_BIT_CLK_CONTROLLER 1
#define I2S_OPT_FRAME_CLK_CONTROLLER 2
struct i2s_config {
 unsigned word_size,channels,format,options,frame_clk_freq;
 struct k_mem_slab *mem_slab;
 unsigned block_size,timeout;
};
static inline int i2s_configure(const struct device *d,int dir,struct i2s_config *c) {
 ARG_UNUSED(d);ARG_UNUSED(dir);ARG_UNUSED(c);return 0;
}
static inline int i2s_write(const struct device *d,void *p,size_t n) {
 ARG_UNUSED(d);ARG_UNUSED(p);ARG_UNUSED(n);return 0;
}
static inline int i2s_trigger(const struct device *d,int dir,int op) {
 ARG_UNUSED(d);ARG_UNUSED(dir);ARG_UNUSED(op);return 0;
}
#endif
