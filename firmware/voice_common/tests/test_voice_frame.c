/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "voice_frame.h"
#include "alarm_button.h"

int main(void)
{
    int16_t samples[320] = {0}, decoded[320];
    uint8_t frame[177];
    memset(frame, 0xA5, sizeof(frame));
    assert(voice_encode(samples, 320, 0x12345678, 0x90abcdef, frame, 176));
    assert(frame[176] == 0xA5);
    assert(voice_u32(frame + 4) == 0x12345678);
    assert(voice_u32(frame + 8) == 0x90abcdef);
    assert(voice_decode(frame, 176, decoded, 320));
    for (int i = 0; i < 320; i++) assert(decoded[i] == 0);

    /* Known IMA nibble vector: at index 0 code 1 adds 1, code 7 adds 11. */
    frame[16] = 0x71;
    assert(voice_decode(frame, 176, decoded, 320));
    assert(decoded[0] == 0 && decoded[1] == 1 && decoded[2] == 12);
    frame[12] = 0; frame[13] = 0x80; frame[14] = 88;
    frame[16] = 0x77; frame[17] = 0xff;
    assert(voice_decode(frame, 176, decoded, 320));
    assert(decoded[1] == 28668 && decoded[2] == 32767);
    assert(decoded[3] == -28669 && decoded[4] == -32768);

    for (int i = 0; i < 320; i++) samples[i] = (int16_t)(8000 * sin(i * 2 * 3.141592653589793 * 440 / 16000));
    assert(voice_encode(samples, 320, 1, 0, frame, 176));
    assert((frame[175] & 0xf0) == 0);
    assert(voice_decode(frame, 176, decoded, 320));
    double error = 0;
    for (int i = 0; i < 320; i++) error += pow((double)samples[i] - decoded[i], 2);
    assert(sqrt(error / 320) < 1100);
    uint8_t repeat[176];
    assert(voice_encode(samples, 320, 1, 0, repeat, 176));
    assert(memcmp(frame, repeat, 176) == 0);

    const int invalid_bytes[] = {0, 1, 2, 3, 14, 15, 175};
    for (unsigned i = 0; i < sizeof(invalid_bytes) / sizeof(invalid_bytes[0]); i++) {
        memcpy(repeat, frame, 176);
        repeat[invalid_bytes[i]] = 255;
        assert(!voice_decode(repeat, 176, decoded, 320));
    }
    assert(!voice_decode(frame, 175, decoded, 320));
    assert(!voice_decode(frame, 176, decoded, 319));
    assert(!voice_encode(samples, 319, 1, 0, frame, 176));
    assert(!voice_encode(samples, 320, 0, 0, frame, 176));
    assert(!voice_encode(samples, 320, 1, 0, frame, 175));
    assert(!voice_encode(NULL, 320, 1, 0, frame, 176));

    uint8_t control[] = {1, 1, 0x78, 0x56, 0x34, 0x12};
    bool start; uint32_t session;
    assert(voice_parse_control(control, 6, &start, &session));
    assert(start && session == 0x12345678);
    control[1] = 0;
    assert(voice_parse_control(control, 6, &start, &session) && !start);
    control[1] = 2;
    assert(!voice_parse_control(control, 6, &start, &session));
    control[1] = 1;
    assert(!voice_parse_control(control, 5, &start, &session));
    memset(control + 2, 0, 4);
    assert(!voice_parse_control(control, 6, &start, &session));
    struct alarm_button_state button = {0};
    assert(!alarm_button_press(&button, true, 100));
    assert(!alarm_button_press(&button, false, 105));
    assert(!alarm_button_press(&button, true, 110));
    assert(!alarm_button_press(&button, true, 139));
    assert(alarm_button_press(&button, true, 140));
    assert(!alarm_button_press(&button, true, 9000));
    assert(!alarm_button_press(&button, false, 9100));
    assert(!alarm_button_press(&button, false, 9130));
    assert(!alarm_button_press(&button, true, 0xfffffff0));
    assert(alarm_button_press(&button, true, 0x0000000e));
    assert(!alarm_button_press(&button, true, 0x000000ff));
    puts("Voice frame: IMA, bounds, controls and BTN4 debounce passed");
    return 0;
}
