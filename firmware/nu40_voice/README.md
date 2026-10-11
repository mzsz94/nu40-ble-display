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
| KIT BTN2 / BTN3 | P1.07 / P1.06, pull-up, active low |
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

## BTN2 recording and BTN3 sending

Keep the HostPC voice page open and enable its NU40 microphone input first.
The existing BLE START command now arms the transport and shows `MIC READY`;
it does not start the PDM microphone. Press BTN2 to record locally, then BTN3
to stop and send the recording through the existing NU87 relay. The microphone
does not transmit captured speech before BTN3. During idle, recording and
ready states, one silent frame per second keeps the existing host session alive.

Recording is bounded to 500 ADPCM frames, approximately 10 seconds. At the
limit it stops and waits for BTN3. BTN2 discards an unsent recording and starts
again; a held button does not repeat. BTN2 and BTN3 are ignored during sending.
The OLED shows recording, limit, sending and error states. BTN3 without a
recording sends nothing. The buffer is volatile and disappears on reset.

After BTN3 the firmware paces stored frames at approximately 20 ms intervals,
then sends 40 silent frames for the unchanged host voice activity detector.
This is intended for a short, single voice command. Long internal pauses may
cause the host to finish an utterance and stop before all stored audio arrives;
the transfer is not an atomic audio clip. Wait until the previous spoken host
response has finished before pressing BTN2 because the existing playback gate
can discard incoming audio. No NU87 or HostPC protocol changes are required.

An armed session lasts at most five minutes. The firmware reserves enough
sequence numbers and nominal time for a full recording before BTN2 and checks
the actual recording again at BTN3. If it cannot fit before the existing host's
1,250-frame renewal boundary or the local deadline, it cancels and displays a
request to stop/start input in HostPC. Delayed BLE delivery may still reach the
deadline during sending; the remainder is cancelled. STOP, a new START,
disconnect, lost subscription, alarm, OTA or an audio/queue failure discards the
current recording. A fresh START only arms; BTN2 is required to capture again.

## BLE extensions

All UUIDs share suffix `-3c0d-4ef3-9d78-574cd1a64087` and service `90e2c000`.

| UUID prefix | Operation | Wire format |
| --- | --- | --- |
| `90e2c001` | OLED ACK notify | Existing KIT protocol |
| `90e2c002` | OLED text write | Existing KIT protocol |
| `90e2c003` | Audio control write with response | version 1, arm 1 / stop 0, nonzero uint32 LE session |
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
accepted. A matching audio frame can be a silent keepalive; it does not prove
the PDM microphone is recording. BTN2 starts capture and BTN3 releases it.

BLE callbacks enqueue or set state only. Dedicated threads perform PDM and
speaker operations. Bounded queues cancel audio on congestion, disconnect,
lost subscription, invalid audio, or session expiry. OLED updates continue during
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
  --create-secrets
```

The first run creates missing secrets with mode 600. Later runs preserve them.
The default signed version follows this application's `VERSION` file (currently
`0.2.2`). Version 0.2.2 restores the effective notification subscription on a
bonded reconnect. Zephyr can restore a saved CCC before the application's
connected callback; writing the same CCC value again need not call its change
callback. Clearing the local subscription flag unconditionally could therefore
reject microphone START or suppress an alarm ACK while OLED writes still work.
Audio and alarm callbacks now read the restored subscription, keep audio
disarmed until a new explicit START, and republish an existing alarm latch.
Use the **same private six-digit pairing-code file for the NU87 build**. Values
are never printed. Keep keys outside the pristine build directory and out of
Git; the repository's `build/` is ignored. Preserve the signing key for future
updates. Changing the signing key requires provisioning its new public key in
MCUboot. BLE SMP image/reset commands require authenticated encryption (L3).
The build directory is mode 700 and published artifacts are mode 600 because
the compiled firmware embeds the private pairing code; treat binaries as private.

The referenced OLED-only application has no MCUboot/SMP OTA service. It needs
one initial USB provisioning step with `build/nu40-voice/artifacts/initial-provision.uf2`.
Other installed firmware versions must be identified before choosing a migration
path. The script **does not flash** a device. The UF2 contains the MCUboot chain
and initial confirmed signed app, preserving the MBR and onboard UF2 bootloader.

This **factory UF2 resets previous firmware, OTA swap metadata, the entire
secondary image slot, NVS settings and BLE pairing bonds**. It explicitly writes
every byte from `0x1000` through `0xdffff`, including erased-value padding. Sparse
UF2 payloads alone would preserve older swap status elsewhere in a flash page
and could resume an incompatible swap. Back up the existing `CURRENT.UF2` and
record `INFO_UF2.TXT` first. Verify `Board-ID: BARAM-NU40`, `SoftDevice: not found`
and the expected memory layout. Readback verification compares UF2 payloads by
flash address; a whole-file hash can differ because readback uses another UF2
layout. Pair again with the locally provisioned code after factory installation.
Subsequent signed SMP OTA updates preserve NVS settings and pairing bonds.

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
python3 -m unittest discover -s firmware/nu40_voice/tests -v
```

The host test runs IMA reference vectors, saturation/length/control validation,
waveform roundtrips and button bounce/hold/clock-wrap cases under ASan/UBSan.
The recording test checks the 500-frame limit, explicit submission, buffer
clearing, session budgets and exact silent keepalives. A second C harness
includes the production `voice.c` callbacks and deferred queue worker; it checks
old queued packets after STOP/restart (including reused session IDs), notify
failure, queue overflow, work submission failure, alarm/OTA/subscription loss
and connection reference ownership. Its Zephyr shims do not emulate the PDM
driver or physical button timing. Factory UF2 tests check erased regions and
confirmation metadata preservation. The graphics test verifies that voice labels
preserve face/horizon pixels and measured values, warnings retain every label,
idle restores the sample number, and text holds expire across clock wrap. It also
covers old warning-to-text transitions and the host's intentional WG1 pause.
The build compiles MCUboot and the app and runs `imgtool verify` on the update.

Earlier 0.1.0 and 0.1.1 USB provisioning observations are recorded in
[VALIDATION.md](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/voice_tactical/VALIDATION.md). The 0.1.1 boot initialized
PDM and BTN2/BTN3/BTN4, but its unstable central connection did not establish
successful physical button capture/transmission.

The 2026-10-11 graphics bench installed the confirmed 0.2.0 factory image over
USB, observed its WG1 boot record and read the active, confirmed image hash
through the authenticated NU87 BLE SMP relay. The operator confirmed that the
OLED face responds to the real IMU. Face/horizon/text commands returned matching
OLED-driver ACKs. See the [bench record](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/docs/demo/OLED-GRAPHICS.md).

This upgrade did not repeat PDM waveform-quality, sustained voice throughput,
audible BTN4 warning or complete OTA upload/test/confirm/revert tests. SMP image
listing alone does not verify a full wireless firmware replacement. Version
0.2.1 integrates button recording with that graphics source; its physical
BTN2/BTN3 behavior and complete voice path require a new bench check.

## WG1 OLED graphics

Firmware 3 adds a measured face/horizon scene renderer. Three-line `@WG1` ASCII
commands use the existing acknowledged TEXT characteristic. The local display
expires after three seconds without a new measurement sequence. The 0.2.1
integration uses a typed local microphone state. While the face or horizon is
visible, only the last right-hand row changes to `MIC READY`, `MIC REC`,
`BTN3 SEND` (recording limit reached), or `MIC SEND`. Measured graphics and
R/P/T values remain visible. `MIC SENT` lasts three seconds and `MIC ERROR`
eight seconds; OFF and expired transient labels restore the sample number.
These local states are not new BLE telemetry: the browser cannot infer the
physical recording state from an audio keepalive or an arm acknowledgement.
In ordinary three-line TEXT mode, the same label occupies the empty fourth
row; an existing fourth payload row is preserved. Without a remote text hold
or cached graphic, the detailed local microphone status screen remains available.

Ordinary remote text, including voice answers, is held for eight seconds while
new WG1 measurements update the cached scene. The host may intentionally stop
ordinary WG1 updates during this period, so ageing the previous sample alone
does not erase the answer after three seconds. At the end of the hold, the
latest cached scene returns; if it has expired, the stale indication appears
until a fresh sample arrives. With no cached scene, the text remains.

A newly received explicit tilt/alarm/stale scene interrupts the text hold.
A newer plain-text command can supersede an older remote warning, allowing a
switch back to text mode. Local BTN4 alarm always has priority. Voice overlays
never change any warning or stale label, and alarm clear restores the selected
text/scene without resuming recording. The OLED ACK confirms the current driver
transfer, not that a cached scene hidden by an answer hold was immediately shown.

See the dashboard usage in
[OLED-GRAPHICS.md](https://github.com/mzsz94/micro-wallaby/blob/feat/military-wallaby-oled-graphics/docs/demo/OLED-GRAPHICS.md).
The host preview is not a camera feed; driver ACK and physical visibility remain
separate observations. The merged source version is 0.2.1.
