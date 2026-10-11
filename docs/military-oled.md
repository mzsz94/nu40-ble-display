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
