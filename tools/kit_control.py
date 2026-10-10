#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Control the NU40 KIT through the existing NU87 SMP relay; NU40 needs power only."""

import argparse
import json
import time
from pathlib import Path

from nu87_capture import select_port
from nu87_ota import DEFAULT_SERIAL, ROOT, Relay

GROUP = 64
ACTIONS = {"beep": 1, "record": 2, "play": 3, "sd": 4, "page": 5, "rgb": 6}
COUNTERS = {"beep": "beeps", "record": "records", "play": "plays"}
UNSIGNED = {"keys", "pressed", "seen", "page", "rgb_mode", "busy", "mic_blocks",
            "mic_errors", "peak", "rms", "samples", "records", "beeps", "plays",
            "sd_saves", "sd_reads", "sd_mounted"}
RESULTS = {"mic_result", "speaker_result", "rgb_result", "sd_result", "audio_result"}


def check_status(status):
    if (not isinstance(status, dict) or type(status.get("schema")) is not int
            or status["schema"] != 1):
        raise ValueError("Unsupported KIT status schema")
    for name in UNSIGNED | RESULTS:
        value = status.get(name)
        minimum = 0 if name in UNSIGNED else -(2**31)
        maximum = 2**32 - 1 if name in UNSIGNED else 2**31 - 1
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f"Invalid KIT status field: {name}")
    limits = {"keys": 15, "pressed": 15, "seen": 15, "page": 2, "rgb_mode": 2,
              "busy": 4, "peak": 32768, "rms": 32768, "samples": 32000, "sd_mounted": 1}
    if any(status[key] > limit for key, limit in limits.items()):
        raise ValueError("KIT status exceeds hardware or buffer bounds")
    if not isinstance(status.get("file"), str) or len(status["file"]) >= 40:
        raise ValueError("Invalid KIT recording path")
    if not isinstance(status.get("version"), str) or len(status["version"]) > 32:
        raise ValueError("Missing KIT firmware version")
    return status


def read_status(relay):
    # Reissuing a read is safe; never replay a possibly executed action here.
    for attempt in range(3):
        try:
            return check_status(relay.request(0, GROUP, 0, {}))
        except RuntimeError as error:
            if str(error) != "NU87 SMP relay failed: -2" or attempt == 2:
                raise
            time.sleep(0.5)


def perform(relay, action, value=0, timeout=12):
    if action not in ACTIONS or type(value) is not int or not 0 <= value <= 2:
        raise ValueError("Unsupported action or value")
    if action not in ("page", "rgb") and value != 0:
        raise ValueError("Only page and rgb accept a value")
    before = read_status(relay)
    reply = relay.request(2, GROUP, 1, {"action": ACTIONS[action], "value": value})
    if reply.get("queued") != ACTIONS[action] or reply.get("value") != value:
        raise ValueError("KIT action acknowledgement does not match the request")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        after = read_status(relay)
        if action in ("page", "rgb"):
            key = "page" if action == "page" else "rgb_mode"
            if after[key] != value:
                raise RuntimeError("KIT display/RGB mode was not applied")
            return before, after
        if after["busy"] == 0:
            if after["audio_result"] != 0:
                raise RuntimeError(f"KIT action failed: {after['audio_result']}")
            if action in COUNTERS:
                counter = COUNTERS[action]
                if after[counter] != ((before[counter] + 1) & 0xffffffff):
                    raise RuntimeError("KIT completion counter did not advance exactly once")
            if action == "record" and after["samples"] != 32000:
                raise RuntimeError("KIT recording is not a complete two-second buffer")
            return before, after
        time.sleep(0.2)
    raise TimeoutError("KIT action did not finish before the deadline")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", *ACTIONS), nargs="?", default="status")
    parser.add_argument("--value", type=int, choices=range(3), default=0)
    parser.add_argument("--serial-number", default=DEFAULT_SERIAL)
    parser.add_argument("--port")
    parser.add_argument("--watch", type=float, default=0, help="status observation seconds")
    parser.add_argument("--output", type=Path, default=ROOT / "build/kit-control")
    args = parser.parse_args(argv)
    if args.watch < 0 or args.watch > 300 or (args.watch and args.action != "status"):
        parser.error("--watch must be 0..300 seconds and is only valid with status")
    if args.action not in ("page", "rgb") and args.value != 0:
        parser.error("--value applies only to page and rgb")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"action": args.action, "value": args.value, "result": "not_started",
              "physical_output": "requires user observation", "states": []}
    try:
        import serial
        from serial.tools import list_ports

        port = select_port(list_ports.comports(), args.serial_number, args.port)
        device = serial.Serial(port=None, baudrate=1500000, timeout=0.1, write_timeout=3)
        device.dtr = False
        device.rts = False
        device.port = port
        report.update(port=port, serial_number=args.serial_number)
        with (args.output / "relay.log").open("w", encoding="utf-8", buffering=1) as log:
            with device:
                relay = Relay(device, log)
                relay.ready()
                try:
                    relay.mode("pause")
                    if args.action == "status":
                        deadline = time.monotonic() + args.watch
                        while True:
                            status = read_status(relay)
                            report["states"].append(status)
                            print(json.dumps(status, ensure_ascii=False), flush=True)
                            if time.monotonic() >= deadline:
                                break
                            time.sleep(0.5)
                    else:
                        before, after = perform(relay, args.action, args.value)
                        report.update(before=before, after=after)
                    report["result"] = "status_received" if args.action == "status" else "completed"
                finally:
                    relay.mode("face")
        return 0
    except Exception as error:
        report.update(result="failed_or_incomplete", error=f"{type(error).__name__}: {error}")
        return 2
    finally:
        (args.output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)
                                               + "\n", encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
