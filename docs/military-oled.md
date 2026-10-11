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
