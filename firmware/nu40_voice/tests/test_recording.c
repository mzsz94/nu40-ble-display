/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "recording.h"
#include "alarm_button.h"
static struct recording clip;
int main(void) {
 int16_t pcm[VOICE_SAMPLES],decoded[VOICE_SAMPLES];uint8_t packet[VOICE_FRAME_SIZE];
 for(unsigned i=0;i<VOICE_SAMPLES;i++)pcm[i]=(int16_t)(i*20);
 assert(!recording_submit(&clip));
 recording_start(&clip);
 assert(!recording_append(&clip,pcm,0));
 for(unsigned i=0;i<RECORDING_MAX_FRAMES;i++){
  assert(recording_append(&clip,pcm,VOICE_SAMPLES));
  assert(!recording_next(&clip,77,0,packet,sizeof(packet)));
 }
 assert(clip.state==RECORDING_READY&&clip.count==500);
 assert(!recording_append(&clip,pcm,VOICE_SAMPLES));
 assert(recording_submit(&clip));assert(!recording_submit(&clip));
 assert(!recording_next(&clip,0,0,packet,sizeof(packet)));
 assert(!recording_next(&clip,77,0,packet,sizeof(packet)-1));
 for(unsigned i=0;i<500;i++){
  assert(recording_next(&clip,77,100+i,packet,sizeof(packet)));
  assert(voice_u32(packet+4)==77&&voice_u32(packet+8)==100+i);
  assert(voice_decode(packet,sizeof(packet),decoded,VOICE_SAMPLES));
  assert(decoded[0]==pcm[0]);
 }
 assert(!recording_next(&clip,77,600,packet,sizeof(packet)));
 recording_start(&clip);assert(clip.state==RECORDING_CAPTURE&&!clip.count&&!clip.cursor);
 assert(recording_append(&clip,pcm,VOICE_SAMPLES));assert(recording_submit(&clip));
 recording_cancel(&clip);assert(clip.state==RECORDING_IDLE&&!clip.count&&!clip.cursor);
 assert(!recording_next(&clip,77,0,packet,sizeof(packet)));
 for(unsigned i=0;i<sizeof(clip.frames);i++)assert(((uint8_t*)clip.frames)[i]==0);
 struct recording_transport link={0};
 assert(!recording_arm(&link,0));assert(recording_arm(&link,77));
 uint32_t old_generation=link.generation;
 assert(clip.state==RECORDING_IDLE);
 assert(recording_session_matches(&link,77,old_generation));
 assert(recording_stop(&link,77));
 assert(!recording_session_matches(&link,77,old_generation));assert(!link.armed);
 assert(recording_arm(&link,88));
 assert(!recording_stop(&link,77));assert(link.armed);
 assert(!recording_session_matches(&link,77,old_generation));
 assert(recording_session_matches(&link,88,link.generation));
 assert(recording_stop(&link,88));assert(!link.armed);
 assert(recording_start_budget(698,278000));assert(!recording_start_budget(699,0));
 assert(!recording_start_budget(0,278001));assert(!recording_start_budget(UINT32_MAX,0));
 assert(recording_send_budget(709,500,0));assert(!recording_send_budget(710,500,0));
 assert(!recording_send_budget(0,0,0));assert(!recording_send_budget(0,501,0));
 assert(!recording_send_budget(UINT32_MAX,1,0));
 assert(recording_send_budget(0,500,288200));assert(!recording_send_budget(0,500,288201));
 memset(packet,0,sizeof(packet));packet[0]='W';packet[1]='A';packet[2]=1;voice_put32(packet+4,88);
 assert(voice_decode(packet,sizeof(packet),decoded,VOICE_SAMPLES));
 for(unsigned i=0;i<VOICE_SAMPLES;i++)assert(decoded[i]==0);
 struct alarm_button_state button={0};
 assert(!alarm_button_press(&button,true,0));assert(!alarm_button_press(&button,false,10));
 assert(!alarm_button_press(&button,true,20));assert(!alarm_button_press(&button,true,49));
 assert(alarm_button_press(&button,true,50));assert(!alarm_button_press(&button,true,100));
 assert(!alarm_button_press(&button,false,110));assert(!alarm_button_press(&button,false,140));
 assert(!alarm_button_press(&button,true,150));assert(alarm_button_press(&button,true,180));
 puts("recording: bounded capture, explicit submit, restart and cancellation passed");
 return 0;
}
