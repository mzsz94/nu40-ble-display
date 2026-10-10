#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read NU40 BLE display logs without sending commands or claiming physical verification."""

import argparse
import json
import re
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[1]
PRODUCT = "NU40DK BLE display v1"
PRODUCTS = frozenset((PRODUCT, "NU40DK BLE display v2", "NU40DK BLE display OTA v3"))
TARGET_SERIAL = "87102B327CB78D38"
USB_VID = 0x2FE3
USB_PID = 0x0004
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
FIELD = re.compile(r"(?:^|\s)([a-z_]+)=([^\s]+)")
STATUS_FIELDS = ("connected", "oled_ready", "rx", "oled_writes", "ack_queued", "errors")
RX_FIELDS = ("seq", "bytes", "oled_result", "ack_result")


def select_port(ports, serial_number=TARGET_SERIAL, port=None):
    """An explicit path still requires exact VID/PID, product and USB serial."""
    if not serial_number or not serial_number.strip():
        raise ValueError("An exact nonempty NU40 USB serial number is required.")
    matches = [entry for entry in ports
               if getattr(entry, "vid", None) == USB_VID
               and getattr(entry, "pid", None) == USB_PID
               and getattr(entry, "product", None) in PRODUCTS
               and getattr(entry, "serial_number", None) == serial_number
               and (port is None or entry.device == port)]
    if len(matches) != 1:
        raise ValueError("Expected exactly one NU40 BLE display with the requested USB identity.")
    return matches[0].device


def number(fields, key):
    value = fields.get(key, "")
    if not re.fullmatch(r"-?[0-9]+", value):
        return None
    return int(value)


def counter_evidence(records, name):
    values = [record[name] for record in records]
    reset = any(second < first for first, second in zip(values, values[1:], strict=False))
    advanced = None if len(values) < 2 or reset else values[-1] > values[0]
    return {"first": values[0] if values else None,
            "last": values[-1] if values else None,
            "advanced": advanced, "decrease_observed": reset}


def summarize(text):
    """Separate complete RX driver results, cumulative counters and visible output."""
    boots, statuses, received, rejected = [], [], [], []
    malformed = 0
    for line in ANSI_ESCAPE.sub("", text).replace("\r", "").splitlines():
        match = re.match(r"LINK\|([A-Z]+)\s+(.*)", line)
        if not match:
            continue
        kind, payload = match.groups()
        # Concurrent printk can overwrite a record tail; never join or invent missing fields.
        payload = payload.split("LINK|", 1)[0]
        pairs = FIELD.findall(payload)
        fields = dict(pairs)
        if len(fields) != len(pairs):
            malformed += 1
            continue
        if kind == "BOOT":
            boots.append({"role": fields.get("role"), "firmware": number(fields, "firmware"),
                          "protocol": number(fields, "protocol"),
                          "oled_init": number(fields, "oled_init")})
        elif kind == "STATUS":
            values = {name: number(fields, name) for name in STATUS_FIELDS}
            valid = (fields.get("role") == "display"
                     and all(value is not None and 0 <= value <= 0xFFFFFFFF
                             for value in values.values())
                     and values["connected"] in (0, 1) and values["oled_ready"] in (0, 1))
            if valid:
                statuses.append(values)
            else:
                malformed += 1
        elif kind == "RX":
            if "rejected" in fields:
                result = number(fields, "rejected")
                queued = number(fields, "queue_result")
                if result is not None and queued is not None:
                    rejected.append({"rejected": result, "queue_result": queued})
                else:
                    malformed += 1
                continue
            values = {name: number(fields, name) for name in RX_FIELDS}
            valid = (all(value is not None for value in values.values())
                     and 0 <= values["seq"] <= 0xFFFF and 1 <= values["bytes"] <= 84)
            if valid:
                received.append(values)
            else:
                malformed += 1
    boot = boots[-1] if boots else None
    if boot is not None:
        recognized = (boot["role"] == "display" and boot["firmware"] in (1, 2, 3)
                      and boot["protocol"] == 1)
        identity_evidence = "BOOT" if recognized else "unsupported or malformed BOOT"
    else:
        recognized = bool(statuses or received)
        identity_evidence = "STATUS/RX signature after missed boot" if recognized else "none"
    if not recognized:
        statuses, received, rejected = [], [], []
    counters = {name: counter_evidence(statuses, name)
                for name in ("rx", "oled_writes", "ack_queued", "errors")}
    successful = [record for record in received if record["oled_result"] == 0]
    acknowledged = [record for record in received if record["ack_result"] == 0]
    last_status = statuses[-1] if statuses else None
    return {
        "firmware_recognized": recognized,
        "firmware_boot_observed": bool(boots),
        "firmware_version": boot["firmware"] if boot else None,
        "firmware_identity_evidence": identity_evidence,
        "oled_init_result": boot["oled_init"] if boot and recognized else None,
        "status_records": len(statuses),
        "latest_status": last_status,
        "connected_observed": any(record["connected"] == 1 for record in statuses),
        "connected_at_last_status": bool(last_status["connected"]) if last_status else None,
        "oled_ready_at_last_status": bool(last_status["oled_ready"]) if last_status else None,
        "counters": counters,
        "rx_counter_advanced": counters["rx"]["advanced"],
        "complete_rx_records": len(received),
        "rx_records": received,
        "successful_oled_driver_records": len(successful),
        "successful_ack_queue_records": len(acknowledged),
        "successful_oled_and_ack_queue_records": sum(
            record["oled_result"] == 0 and record["ack_result"] == 0 for record in received),
        "rx_driver_error_observed": any(record["oled_result"] != 0 or record["ack_result"] != 0
                                        for record in received),
        "rejected_rx_records": rejected,
        "malformed_or_incomplete_records": malformed,
        "oled_visible": "unverified; human observation required",
        "ack_delivery": "unverified; ack_result=0 records notification queuing only",
        "ble_peer_identity": "unverified by this NU40-only capture",
        "overall_physical_result": "unverified; logs are driver and protocol evidence only",
    }


def duration(value):
    try:
        seconds = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("--seconds must be an integer between 20 and 300") from exc
    if not 20 <= seconds <= 300:
        raise argparse.ArgumentTypeError("--seconds must be between 20 and 300")
    return seconds


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial path; all identity checks still apply")
    parser.add_argument("--serial-number", default=TARGET_SERIAL, help="exact NU40 USB serial")
    parser.add_argument("--seconds", type=duration, default=30)
    parser.add_argument("--output", type=Path, default=WORKSPACE / "build/ble-display.log")
    args = parser.parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    transcript = []
    device_buffer = ""
    selected_port = None
    selected_product = None
    capture_error = None
    interrupted = False
    exit_code = 2
    output_bytes = 0

    with args.output.open("w", encoding="utf-8", buffering=1) as logfile:
        def emit(message):
            nonlocal output_bytes
            transcript.append(message)
            logfile.write(message)
            output_bytes += len(message)
            print(message, end="", flush=True)

        def note(message):
            nonlocal device_buffer
            if device_buffer:
                emit(device_buffer + "\n")
                device_buffer = ""
            emit(f"[HOST] {message}\n")

        try:
            import serial
            from serial.tools import list_ports

            ports = list_ports.comports()
            selected_port = select_port(ports, args.serial_number, args.port)
            selected_product = next(entry.product for entry in ports
                                    if entry.device == selected_port)
            note(f"Opening {selected_port} at 115200 baud; exact NU40 BLE display USB identity.")
            note("Read-only capture; no serial writes, commands or firmware changes.")
            note("OLED 글자가 보이는지 직접 확인하세요. NU87이 아직 연결되지 않아도 캡처합니다.")
            note("Exit 0 means recognized logs were captured; physical output remains unverified.")
            with serial.Serial(selected_port, 115200, timeout=0.1) as port:
                end = time.monotonic() + args.seconds
                while time.monotonic() < end:
                    chunk = port.read(min(max(port.in_waiting, 1), 4096))
                    if not chunk:
                        continue
                    device_buffer += chunk.decode("utf-8", errors="replace")
                    while "\n" in device_buffer:
                        line, device_buffer = device_buffer.split("\n", 1)
                        emit(line + "\n")
                    if output_bytes + len(device_buffer) > 2 * 1024 * 1024:
                        raise RuntimeError("Stopped after 2 MiB of device output.")
            exit_code = 0
        except KeyboardInterrupt:
            interrupted = True
            capture_error = "Interrupted by user; partial evidence retained."
            exit_code = 130
            note(capture_error)
        except Exception as error:
            capture_error = f"{type(error).__name__}: {error}"
            note(f"Capture error: {capture_error}")
        finally:
            if device_buffer:
                emit(device_buffer + "\n")
                device_buffer = ""
            result = summarize("".join(transcript))
            if exit_code == 0 and not result["firmware_recognized"]:
                exit_code = 1
                note("BLE display logs not recognized; USB name alone cannot confirm firmware.")
            result.update({
                "port": selected_port,
                "usb_identity": {"vid": "2fe3", "pid": "0004", "product": selected_product,
                                 "serial_number": args.serial_number,
                                 "matched": selected_port is not None},
                "baud": 115200,
                "serial_writes": False,
                "requested_seconds": args.seconds,
                "capture_completed": exit_code in (0, 1),
                "interrupted": interrupted,
                "capture_error": capture_error,
                "exit_code": exit_code,
            })
            summary_path = args.output.with_suffix(".json")
            summary_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                                    encoding="utf-8")
            note(f"Log saved: {args.output}")
            note(f"Evidence summary: {summary_path}")
            print(json.dumps(result, ensure_ascii=False, indent=2))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
