/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_RECORDING_H
#define WALLABY_RECORDING_H
#include "voice_frame.h"

#define RECORDING_MAX_FRAMES 500U /* 10 seconds at 16 kHz, 88 kB ADPCM. */
#define RECORDING_TAIL_FRAMES 40U
#define RECORDING_HOST_RENEW 1250U
#define RECORDING_ARM_MS 300000U
struct recording_transport { uint32_t session, generation, sequence; bool armed; };
static inline bool recording_arm(struct recording_transport *link,uint32_t session) {
 if(!session)return false;
 link->session=session;link->generation++;link->sequence=0;link->armed=true;return true;
}
static inline bool recording_stop(struct recording_transport *link,uint32_t session) {
 if(!session||session!=link->session)return false;
 link->armed=false;link->generation++;return true;
}
static inline bool recording_session_matches(const struct recording_transport *link,uint32_t session,uint32_t generation) {
 return link->armed&&session==link->session&&generation==link->generation;
}
static inline bool recording_start_budget(uint32_t sequence,uint32_t elapsed_ms) {
 return sequence<RECORDING_HOST_RENEW&&sequence+RECORDING_MAX_FRAMES+RECORDING_TAIL_FRAMES+11U<RECORDING_HOST_RENEW&&elapsed_ms<=RECORDING_ARM_MS-22000U;
}
static inline bool recording_send_budget(uint32_t sequence,uint32_t count,uint32_t elapsed_ms) {
 return sequence<RECORDING_HOST_RENEW&&count>0&&count<=RECORDING_MAX_FRAMES&&sequence+count+RECORDING_TAIL_FRAMES<RECORDING_HOST_RENEW&&
        elapsed_ms<=RECORDING_ARM_MS-(count+RECORDING_TAIL_FRAMES)*20U-1000U;
}
enum recording_state { RECORDING_IDLE, RECORDING_CAPTURE, RECORDING_READY, RECORDING_SEND };
struct recording {
 enum recording_state state;
 uint16_t count, cursor;
 uint8_t frames[RECORDING_MAX_FRAMES][VOICE_FRAME_SIZE];
};

/* These functions belong to the microphone thread, never a BLE callback. */
static inline void recording_cancel(struct recording *clip) {
 memset(clip,0,sizeof(*clip));
}
static inline void recording_start(struct recording *clip) {
 recording_cancel(clip);clip->state=RECORDING_CAPTURE;
}
static inline bool recording_append(struct recording *clip,const int16_t *samples,size_t count) {
 if(clip->state!=RECORDING_CAPTURE||clip->count>=RECORDING_MAX_FRAMES)return false;
 if(!voice_encode(samples,count,1,clip->count,clip->frames[clip->count],VOICE_FRAME_SIZE))return false;
 clip->count++;
 /* Reaching the limit stops recording but never releases audio for sending. */
 if(clip->count==RECORDING_MAX_FRAMES)clip->state=RECORDING_READY;
 return true;
}
static inline bool recording_submit(struct recording *clip) {
 if((clip->state!=RECORDING_CAPTURE&&clip->state!=RECORDING_READY)||!clip->count)return false;
 clip->state=RECORDING_SEND;clip->cursor=0;return true;
}
static inline bool recording_next(struct recording *clip,uint32_t session,uint32_t sequence,uint8_t *out,size_t capacity) {
 if(clip->state!=RECORDING_SEND||clip->cursor>=clip->count||!session||!out||capacity<VOICE_FRAME_SIZE)return false;
 memcpy(out,clip->frames[clip->cursor],VOICE_FRAME_SIZE);
 voice_put32(out+4,session);voice_put32(out+8,sequence);clip->cursor++;
 return true;
}
#endif
