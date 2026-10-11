# Military Wallaby OLED graphics

NU40 renders a face, gravity horizon and warning icons on the KIT's 128×32
SSD1306-compatible OLED. A real NU87 I2C IMU supplies roll, pitch, tilt and sample
numbers through Wi-Fi → HostPC → NU87 BLE → NU40. No video stream is required.

The shared implementation is `shared/wallaby_graphics.h`. It accepts a bounded
three-line ASCII snapshot on the existing acknowledged TEXT characteristic:

```text
@WG1 H L
-12 11 16
202748
```

Views: `F` face, `H` horizon. States: `L` live, `R` reference needed, `C` calibrating,
`P` virtual patrol request, `S` virtual stop, `A` tilt warning, `B` training alarm,
`X` stale. Numbers are actual whole-degree R/P/T and the NU87 measurement sequence.
Unknown versions, out-of-range values and trailing data are rejected. Ordinary
voice/result text still uses the existing text renderer.

The left 64 pixels contain graphics; four 10-column labels use the right side.
Cosmetic blinking runs locally at a roughly 120ms drawing interval. It does not
generate sensor measurements. A local three-second timeout blanks attitude data;
repeated sample numbers do not renew the deadline. Latched warnings remain visible.
The full sequence stays on HostPC; the OLED shows its last nine digits.

Two applications are provided:

- `nu40_ble_display`: existing complete KIT example with local keys, RGB,
  recording/playback, SD and its original OTA configuration. WG1 is added to the
  remote page; a tilt warning takes priority over local pages. Existing features
  are preserved. A received ACK may accompany a local page if the operator selected it.
- `firmware/nu40_voice`: Military Wallaby microphone/alarm/graphics application,
  mirrored from `micro-wallaby`. It provides live BLE microphone audio, a BTN4
  training alarm and authenticated, signed MCUboot OTA. It is a separate application,
  not a claim that live voice and every feature of the complete KIT example run
  together. Its pins, flash layout and signing procedure are in its README.

The corresponding dashboard, real-IMU controls and virtual driving scenario are
in [micro-wallaby / feat/military-wallaby-oled-graphics](https://github.com/mzsz94/micro-wallaby/tree/feat/military-wallaby-oled-graphics),
with usage in `docs/demo/OLED-GRAPHICS.md`. The local page is
<http://127.0.0.1:8767/hardware.html>.

Test the actual parser and rasterizer with sanitizers:

```bash
python3 -m unittest discover -s firmware/nu40_voice/tests
```

The browser preview is a host rendering of a requested scene, not a photographed
OLED or hardware proof. A matching ACK confirms the driver transfer; physical
visibility requires observation. Build output, firmware images and private
credentials/signing keys are not tracked.

## 2026-10-11 physical bench

The Military Wallaby voice application was installed on the actual NU40 over
USB after backing up its previous application. Its boot log reported firmware 3,
WG1 graphics and OLED initialization result 0. The operator then confirmed that
the visible face responds when the actual NU87 IMU is tilted. Face, horizon and
text commands also produced matching OLED-driver ACKs through Wi-Fi and BLE.
The complete KIT example was compiled, but was not installed for this bench.

Both boards now use the same private pairing code. Authenticated SMP image
listing returned active, confirmed version `0.2.0` with image hash
`543863dfe6a4f89f3e0895ca75fa45de3f95511ee2111eb630260104acfa41e8`.
This version was provisioned over USB; this check is not a new full OTA
upload/test/confirm/revert test. Microphone waveform quality and the audible
BTN4 alarm were not retested for this graphics upgrade.

The host now polls input independently of output ACK waits and serializes
requests to the embedded single-client HTTP listener. Stale input still blocks
virtual motion. Actual evidence and operating instructions are in the linked
`micro-wallaby` [bench guide](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/docs/demo/OLED-GRAPHICS.md).
The guide includes a real-time GIF/MP4 of actual IMU/OLED-ACK screen selection
and virtual patrol, W-key driving and Space stopping. The sensor remains on the
desk during that UI check; the clip does not claim a new physical tilt-warning
test or a camera view of the OLED. The host regression suites passed 134 Python
and 52 JavaScript tests, and the real browser check reported no JavaScript errors.

## 0.2.2 wireless update and reconnect bench

Collaborator sz's button recording and voice-overlay changes are integrated.
Version `0.2.2` also fixes connection callbacks that cleared a restored, bonded
CCC subscription. Both microphone and alarm callbacks now read the actual GATT
subscription; audio still requires explicit START and physical button control.
The native regression includes the production callbacks, fails with the old
implementation and passes with the fix. All 10 NU40 tests passed with ASan/UBSan
in the C harnesses.

The real NU40 received the signed `0.2.2` image through the NU87 BLE SMP relay.
After a test boot, OLED and alarm-clear ACKs and microphone START/STOP succeeded.
Only then was the exact image explicitly confirmed. A further software reset
preserved active, confirmed version `0.2.2` and repeated those checks successfully.
The MCUboot image hash is
`9f46926b733401b928594d3b41eec0938f35078378cf80af77864a73e1024568`.
An earlier unconfirmed `0.2.1` test reverted to `0.2.0` on software reset.

Each audio check received three silent armed frames with no detected sequence
loss. This verifies transport, not BTN2/BTN3 recording, acoustic capture or speech
recognition. The audible BTN4 alarm and power loss during OTA remain untested
for this upgrade. Intermittent BLE timeout `0x08` remains recorded; this is not
a claim of continuous wireless stability.

The [latest GIF, MP4 and evidence](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/docs/demo/OLED-GRAPHICS.md)
show a 24.92-second actual browser check after the confirmed reset: real IMU
input, face/horizon/text ACKs, virtual patrol, W-key driving and Space stopping.
The IMU stayed on the desk, and the OLED preview is host-rendered.

## 응답 개선 소스 0.2.3

0.2.3은 실측 자세 사이의 160ms 화면 보간과 60ms 로컬 렌더링을 추가한다.
±180°는 짧은 회전 경로를 사용하고 얼굴 눈동자는 sin(roll)로 연결된다.
새 값은 있지만 중력이 불안정한 상태는 `ATT MOVING`, 실제 만료는
`LINK STALE`이다. 경보와 만료는 즉시 적용하며 기존 3초 만료를 늘리지 않는다.
HostPC의 그래픽 전송 최소 간격은 1.2초에서 0.3초로 줄었다. 이는 설정값이며
실제 무선 응답 시간은 별도로 측정한다.

직전 0.2.2 + NU87 edc5094 검사에서는 실제 4초 supervision 협상 후
301.96초 동안 BLE 단절·OLED 오류가 없었다. 그러나 Host 입력 지연으로
16/1500개 관측이 기존 1.5초 freshness 기준을 넘었고, 사용자는 180° 부근
STALE과 지연을 보고했다. 따라서 연결 유지와 자세 유효성을 구분한다.
0.2.3의 BLE SMP 설치·시험 부팅·동작 검사·명시적 확정을 실제로 완료했다.

활성·확정 해시: `bd6e555d5a542d0a3efae2c61b95230794d838cb09d64e26188957a0a233e58a`.
서명 파일 SHA256: `ee309964f3bf2caf6700d77b1a22d826d36bf63b930c5f31fefa3fea9a0336b1`.
FLASH 239,372B / RAM 168,464B, 소유·미러 각각 10개 네이티브/프로비저닝 검사 통과.
실물 OLED·경보 해제 ACK와 준비 무음 오디오 프레임 3개를 확인했다.

이전 0.2.2의 ACK 간격 중앙값 1.259초에서, 0.2.3 첫 검사 0.533초, 최종 Host
설정 검사 0.496초로 줄었다. 최종 120.63초 동안 새 BLE 단절과 출력·I2C 오류는 0회였다.
다만 Host freshness 초과는 12/600개, 최대 입력 공백은 2.20초로 남아 있다.
물리 180° 회전·체감 반응은 아직 사용자 응답을 받지 못했으며 카메라 지연 측정도 아니다.
[전체 검증 보고서](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/docs/demo/RESPONSIVE-GRAPHICS.md)를 참고한다.
