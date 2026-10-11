/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_VOICE_FRAME_H
#define WALLABY_VOICE_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define VOICE_SAMPLE_RATE 16000U
#define VOICE_SAMPLES 320U
#define VOICE_FRAME_SIZE 176U
#define VOICE_CONTROL_SIZE 6U
#define VOICE_MIN_MTU 179U
#define VOICE_MAX_FRAMES 1500U
#define VOICE_UUID_CONTROL_VAL BT_UUID_128_ENCODE(0x90e2c003,0x3c0d,0x4ef3,0x9d78,0x574cd1a64087)
#define VOICE_UUID_AUDIO_VAL BT_UUID_128_ENCODE(0x90e2c004,0x3c0d,0x4ef3,0x9d78,0x574cd1a64087)
static const int voice_ima_steps[89] = {
 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,
 60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
 337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
 1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
 5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
 16818,18500,20350,22385,24623,27086,29794,32767};
static const int voice_ima_adjust[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static inline uint32_t voice_u32(const uint8_t *p) {
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline void voice_put32(uint8_t *p,uint32_t v) {
 for(size_t i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8));
}
static inline int voice_clamp(int v,int low,int high) {return v<low?low:v>high?high:v;}
static inline bool voice_parse_control(const uint8_t *p,size_t n,bool *start,uint32_t *session) {
 if(!p||!start||!session||n!=6||p[0]!=1||p[1]>1||!voice_u32(p+2)) return false;
 *start=p[1]==1; *session=voice_u32(p+2); return true;
}
/* Independent block, sample 0 predictor and index 0, low nibble first. */
static inline bool voice_encode(const int16_t *pcm,size_t n,uint32_t session,uint32_t seq,uint8_t *out,size_t cap) {
 if(!pcm||!out||n!=320||!session||cap<176) return false;
 memset(out,0,176); out[0]='W';out[1]='A';out[2]=1;
 voice_put32(out+4,session);voice_put32(out+8,seq);
 int pred=pcm[0],idx=0;out[12]=(uint8_t)pred;out[13]=(uint8_t)((uint16_t)pred>>8);
 for(size_t i=1;i<320;i++) {
  int step=voice_ima_steps[idx],diff=(int)pcm[i]-pred,change=step>>3;
  uint8_t code=diff<0?8:0;if(diff<0)diff=-diff;
  if(diff>=step){code|=4;diff-=step;change+=step;}
  if(diff>=(step>>1)){code|=2;diff-=step>>1;change+=step>>1;}
  if(diff>=(step>>2)){code|=1;change+=step>>2;}
  pred=voice_clamp(pred+((code&8)?-change:change),-32768,32767);
  idx=voice_clamp(idx+voice_ima_adjust[code],0,88);
  out[16+(i-1)/2]|=code<<(((i-1)%2)*4);
 }return true;
}
static inline bool voice_decode(const uint8_t *in,size_t n,int16_t *pcm,size_t cap) {
 if(!in||!pcm||n!=176||cap<320||in[0]!='W'||in[1]!='A'||in[2]!=1||in[3]||in[15]||in[14]>88||!voice_u32(in+4)||(in[175]&0xf0))return false;
 int pred=(int16_t)((uint16_t)in[12]|((uint16_t)in[13]<<8)),idx=in[14];pcm[0]=pred;
 for(size_t i=1;i<320;i++) {
  uint8_t code=(in[16+(i-1)/2]>>(((i-1)%2)*4))&15;
  int step=voice_ima_steps[idx],change=step>>3;
  if(code&4)change+=step;
  if(code&2)change+=step>>1;
  if(code&1)change+=step>>2;
  pred=voice_clamp(pred+((code&8)?-change:change),-32768,32767);
  idx=voice_clamp(idx+voice_ima_adjust[code],0,88);pcm[i]=pred;
 }return true;
}
#endif
