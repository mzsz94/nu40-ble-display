# military-wallaby NU40 control tower

NU40DK-KIT is the control tower: its PDM microphone records speech, OLED shows
results, and BTN4 simulates an explosive-detection event. The NU87 Wallaby node
relays audio/events to the HostPC analysis server and receives its commands.
BTN4 does not detect a physical substance. A local press immediately latches an
alert and starts a finite speaker warning, including when BLE is disconnected.

This application extends the Apache-2.0
[NU40 BLE display](https://github.com/mzsz94/nu40-ble-display/tree/230a1300)
reference. Its original `NU40-KIT-DISPLAY` advertised name, service UUID, OLED
frame format and ACK are preserved. The copied font retains `LICENSE.font`.

## Verified reference wiring

The manufacturer's
[PDM/SD example](https://github.com/Nu-makers/nucode_nu40_PDM_SD/tree/5daecad68a7bedefcd316fd1c7cfe2b3f74d5c7c)
uses the following KIT pins. Arduino
[`setPins(data, clock, power)`](https://github.com/Nucode01/Adafruit_nRF52_Arduino/blob/master/libraries/PDM/src/PDM.h)
and the [NU40 pin map](https://github.com/Nucode01/Adafruit_nRF52_Arduino/blob/master/variants/nu40dk_nrf52840/variant.cpp)
confirm direct port numbering. These are reference-board assignments; the
attached physical board and its revision still require a bench check.

| Function | NU40 pin |
| --- | --- |
| PDM data / clock | P0.07 / P0.05 |
| OLED SDA / SCL | P0.22 / P0.21 |
| MAX98357A DIN / BCLK / LRC | P0.19 / P1.09 / P1.08 |
| Amplifier SD enable | P1.10, active high |
| KIT BTN4 | P1.05, pull-up, active low |

The microphone is 16 kHz mono PCM16. `CONFIG_WALLABY_PDM_RIGHT_CHANNEL=y`
selects the other microphone channel when required by a verified board revision.
Use `build.py --overlay path.overlay` for an additional verified pin overlay.
The speaker plays an approximately 1 kHz, low-amplitude warning, 200 ms on/off,
for at most 10 seconds. The alert stays latched after sound ends. An explicit
host clear silences the amplifier and releases the microphone gate; speech and
robot movement never resume automatically. A BLE disconnect silences an active
tone while preserving the alert latch. The next BTN4 press requires release and
a cleared latch. Debouncing requires 30 ms of stable input.

## BLE extensions

All UUIDs share suffix `-3c0d-4ef3-9d78-574cd1a64087` and service `90e2c000`.

| UUID prefix | Operation | Wire format |
| --- | --- | --- |
| `90e2c001` | OLED ACK notify | Existing KIT protocol |
| `90e2c002` | OLED text write | Existing KIT protocol |
| `90e2c003` | Audio control write with response | version 1, start 1 / stop 0, nonzero uint32 LE session |
| `90e2c004` | Audio notify | Exactly 176 bytes described below |
| `90e2c005` | Alarm control write with response | version 1, on 1 / clear 0, nonzero uint32 LE sequence |
| `90e2c006` | Alarm ACK/event notify | version 1, latched 0/1, uint32 LE sequence, status, source |

Alarm status is 0 for driver acceptance, 1 unsupported, 2 driver failure.
Source 0 is a host command; source 1 and sequence 0 identify a BTN4 event.
A BTN4 event is published even when speaker output fails. Driver acceptance
does not prove audible sound. A latched alert is republished on subscription.

Audio frame: `WA`, version 1, flags 0, session uint32 LE, sequence uint32 LE,
initial predictor int16 LE, IMA index uint8 (0–88), reserved 0, then 160 bytes
of IMA ADPCM. Sample zero is the predictor; 319 remaining samples are encoded
low nibble first, with the final high nibble zero. Each frame independently
decodes to 320 samples / 20 ms. Sequence begins at zero. ATT MTU must be at
least 179; this firmware requests 247. A control ACK means the request was
accepted; the first matching audio frame confirms capture began.

BLE callbacks enqueue or set state only. Dedicated threads perform PDM and
speaker operations. Bounded queues stop capture on congestion, disconnect,
lost subscription, invalid audio, or 30 seconds. OLED updates continue during
capture. An alarm or OTA upload suppresses further microphone frames and blocks
new recordings. A fresh explicit start is required afterward.

## Build, provisioning, and subsequent OTA

The build needs an existing Zephyr west workspace, MCUboot, Zephyr SDK, and a
Python environment with west, cryptography and intelhex. The 0.2.0 NU40 graphics
build used Zephyr `8dafb9a897da`, SDK 1.0.1 and Python 3.12. Earlier validation
used `e70694102ad6`; the NU87 gateway still uses its separate, pinned workspace.

```bash
python firmware/nu40_voice/build.py \
  --workspace /path/to/west-workspace \
  --python /path/to/zephyr-venv/bin/python \
  --sdk /path/to/zephyr-sdk \
  --key build/private/nu40-signing.pem \
  --pairing-code-file build/private/ble-passkey.txt \
  --create-secrets --version 0.1.0
```

The first run creates missing secrets with mode 600. Later runs preserve them.
Use the **same private six-digit pairing-code file for the NU87 build**. Values
are never printed. Keep keys outside the pristine build directory and out of
Git; the repository's `build/` is ignored. Preserve the signing key for future
updates. Changing the signing key requires provisioning its new public key in
MCUboot. BLE SMP image/reset commands require authenticated encryption (L3).

The existing OLED-only application has no MCUboot/SMP OTA service. It needs
one initial USB provisioning step with `build/nu40-voice/artifacts/initial-provision.uf2`.
The script **does not flash** a device. The UF2 contains the MCUboot chain and
initial confirmed signed app, preserving the MBR and onboard UF2 bootloader.

| Board region | Range |
| --- | --- |
| Preserved MBR | `0x00000–0x00fff` |
| MCUboot | `0x01000–0x0ffff` |
| App slot 0 | `0x10000–0x75fff` |
| App slot 1 | `0x76000–0xdbfff` |
| Settings | `0xdc000–0xdffff` |
| Preserved onboard UF2 | `0xe0000+` |

Subsequent updates use `artifacts/nu40-update.signed.bin` through the NU87 SMP
relay. The image is ECDSA-P256 signed and **unconfirmed**. Upload, select a test
boot, reboot, check the expected image hash and device behavior, then explicitly
confirm it from the host. MCUboot uses swap-move without scratch; an unconfirmed
test image reverts on the following reset. The application never auto-confirms.
`manifest.json` contains the artifact hashes and version. Do not upload the
unsigned `zephyr.bin` or factory UF2 through SMP.

## Validation and remaining bench work

```bash
python3 -m unittest discover -s firmware/voice_common/tests -v
```

The host test runs IMA reference vectors, saturation/length/control validation,
waveform roundtrips and BTN4 bounce/hold/clock-wrap cases under ASan/UBSan.
The build compiles MCUboot and the app and runs `imgtool verify` on the update.
The 2026-10-11 graphics bench installed the confirmed 0.2.0 factory image over
USB, observed its WG1 boot record and read the active, confirmed image hash
through the authenticated NU87 BLE SMP relay. The operator confirmed that the
OLED face responds to the real IMU. Face/horizon/text commands returned matching
OLED-driver ACKs. See the [bench record](../../docs/military-oled.md).

This upgrade did not repeat PDM waveform-quality, sustained voice throughput,
audible BTN4 warning or complete OTA upload/test/confirm/revert tests. SMP image
listing alone does not verify a full wireless firmware replacement.

## WG1 OLED graphics

Firmware 3 adds a measured face/horizon scene renderer. Three-line `@WG1` ASCII
commands use the existing acknowledged TEXT characteristic. The local display
expires after three seconds without a new measurement sequence. Ordinary voice
text still renders normally; a local BTN4 training alarm has priority.

See the dashboard usage in
[OLED-GRAPHICS.md](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/docs/demo/OLED-GRAPHICS.md).
The host preview is not a camera feed; driver ACK and physical visibility remain
separate observations. The version for this upgrade is 0.2.0.
