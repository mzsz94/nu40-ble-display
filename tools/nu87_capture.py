#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture NU87 BLE evidence, with an optional exact diagnostic RAM prompt confirmation."""

import argparse
import json
import re
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[1]
BAUD = 1500000
USB_VID = 0x10C4
USB_PID = 0xEA60
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
FIELD = re.compile(r"(?:^|\s)([a-z_]+)=([^\s]+)")
EVENT_FIELDS = {"BLE": "enable_result", "CONNECT": "result", "SUBSCRIBE": "result",
                "SEND": "result", "DISCONNECT": "reason"}
SCAN_COUNTERS = ("uptime_ms", "callbacks", "connectable", "uuid_match", "candidate_queued",
                 "candidate_dropped")
SCAN_FLAGS = ("waiting", "connected", "ready")
RADIO_DIAGNOSTIC = "v4-radio-preinit"
NATIVE_DIAGNOSTIC = "arduino-3.1.9"
RADIO_WARNING = "Efuse empty! Wifi performance may be affected."
RADIO_CONTINUE = "Press any key to ignore and continue"
RADIO_PROMPT = RADIO_WARNING + " " + RADIO_CONTINUE
RADIO_LOG_PREFIX = "[WLAN-E] "


class RadioRamPrompt:
    """Observe only device bytes; a complete exact boot must precede the prompt."""

    def __init__(self):
        self.pending = ""
        self.previous_line = ""
        self.boot_qualified = False
        self.firmware_confirmed = False
        self.qualified_boot_observed = False
        self.prompt_observed = False
        self.completed_prompt_observed = False
        self.prompt_observed_without_qualified_boot = False
        self.prompt_observed_before_qualified_boot = False

    def is_prompt(self, line):
        # The native Wi-Fi logger adds this exact prefix on the real NU87.
        # Accept it only for prompt text; BOOT identity remains an exact raw line.
        message = line.removeprefix(RADIO_LOG_PREFIX)
        previous = self.previous_line.removeprefix(RADIO_LOG_PREFIX)
        return (message == RADIO_PROMPT
                or (previous == RADIO_WARNING and message == RADIO_CONTINUE))

    def record_unqualified_prompt(self):
        self.prompt_observed_without_qualified_boot = True
        if not self.qualified_boot_observed:
            self.prompt_observed_before_qualified_boot = True

    def feed(self, text):
        self.pending += text
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            clean = ANSI_ESCAPE.sub("", line).strip("\r")
            if clean and (clean.startswith("NU87|BOOT") or "NU87|BOOT".startswith(clean)):
                self.boot_qualified = clean == (
                    "NU87|BOOT app=ble_display firmware=1 protocol=1 "
                    f"baud={BAUD} diagnostic={RADIO_DIAGNOSTIC}"
                )
                self.qualified_boot_observed |= self.boot_qualified
                self.previous_line = ""
                self.prompt_observed = False
                self.completed_prompt_observed = False
            else:
                if self.is_prompt(clean):
                    if self.boot_qualified:
                        self.completed_prompt_observed = True
                    else:
                        self.record_unqualified_prompt()
                self.previous_line = clean
        pending = ANSI_ESCAPE.sub("", self.pending).strip("\r")
        # A new partial BOOT cannot inherit permission from an earlier session.
        partial_boot = bool(pending) and (pending.startswith("NU87|BOOT")
                                         or "NU87|BOOT".startswith(pending))
        self.firmware_confirmed = self.boot_qualified and not partial_boot
        pending_prompt = self.is_prompt(pending)
        if pending_prompt and not self.firmware_confirmed:
            self.record_unqualified_prompt()
        self.prompt_observed = self.firmware_confirmed and (
            self.completed_prompt_observed or pending_prompt
        )
        return self.firmware_confirmed and self.prompt_observed


def select_port(ports, serial_number, port=None):
    """Require a unique exact bridge serial, including when a path is supplied."""
    if not serial_number or not serial_number.strip():
        raise ValueError("An exact nonempty CP210x USB --serial-number is required.")
    matches = [entry for entry in ports
               if getattr(entry, "vid", None) == USB_VID
               and getattr(entry, "pid", None) == USB_PID
               and getattr(entry, "serial_number", None) == serial_number]
    if len(matches) != 1 or (port is not None and matches[0].device != port):
        raise ValueError("Expected one 10c4:ea60 bridge with the exact requested serial/path.")
    return matches[0].device


def number(fields, key):
    value = fields.get(key, "")
    return int(value) if re.fullmatch(r"-?[0-9]+", value) else None


def native_boot(boot):
    return bool(boot and boot["app"] == "ble_display_native"
                and boot["diagnostic"] == NATIVE_DIAGNOSTIC and boot["baud"] == BAUD
                and boot["firmware"] in (1, 2, 3) and boot["protocol"] == 1)


def summarize(text):
    """Record software events; ACK receipt and SEND=0 do not verify visible pixels."""
    boots, events, scan_starts, scan_observations = [], [], [], []
    malformed = 0
    for line in ANSI_ESCAPE.sub("", text).replace("\r", "").splitlines():
        # Reset can emit NUL padding before the first printable LOGUART record.
        # Strip only that leading padding, retaining embedded bytes and prefixes.
        line = line.lstrip("\x00")
        match = re.match(r"NU87\|([A-Z]+)\s+(.*)", line)
        if not match:
            continue
        kind, payload = match.groups()
        # printk interleaving can truncate the tail. Do not combine fragments.
        pairs = FIELD.findall(payload.split("NU87|", 1)[0])
        fields = dict(pairs)
        if len(fields) != len(pairs):
            malformed += 1
            continue
        if kind == "BOOT":
            # Attribute only records after the latest boot to its firmware identity.
            events.clear()
            scan_starts.clear()
            scan_observations.clear()
            boots.append({"app": fields.get("app"), "firmware": number(fields, "firmware"),
                          "protocol": number(fields, "protocol"), "baud": number(fields, "baud"),
                          "diagnostic": fields.get("diagnostic")})
        elif kind == "SCAN":
            if "start_result" in fields:
                result = number(fields, "start_result")
                if result is None or not -0x80000000 <= result <= 0x7FFFFFFF:
                    malformed += 1
                    continue
                scan_starts.append(result)
            else:
                observation = {key: number(fields, key) for key in SCAN_COUNTERS + SCAN_FLAGS}
                if (any(value is None or not 0 <= value <= 0xFFFFFFFF
                        for value in observation.values())
                        or any(observation[key] not in (0, 1) for key in SCAN_FLAGS)):
                    malformed += 1
                    continue
                scan_observations.append(observation)
        elif kind in EVENT_FIELDS:
            name = EVENT_FIELDS[kind]
            value = number(fields, name)
            limit = 255 if kind in ("CONNECT", "SUBSCRIBE", "DISCONNECT") else 0x7FFFFFFF
            lower = 0 if kind in ("CONNECT", "SUBSCRIBE", "DISCONNECT") else -0x80000000
            # The native sketch reports signed app errors, whereas Zephyr's
            # connection/subscription callbacks report unsigned HCI/ATT status.
            if kind in ("CONNECT", "SUBSCRIBE") and native_boot(boots[-1] if boots else None):
                lower, limit = -0x80000000, 0x7FFFFFFF
            if value is None or not lower <= value <= limit:
                malformed += 1
                continue
            events.append({"event": kind, name: value})
        elif kind == "ACK":
            sequence, status = number(fields, "seq"), number(fields, "status")
            uptime = number(fields, "at_ms")
            if (sequence is None or not 0 <= sequence <= 0xFFFF
                    or status is None or not 0 <= status <= 3
                    or uptime is None or not 0 <= uptime <= 0xFFFFFFFF):
                malformed += 1
                continue
            events.append({"event": kind, "seq": sequence, "status": status,
                           "at_ms": uptime})
        elif kind == "LINK" and native_boot(boots[-1] if boots else None):
            if fields == {"state": "disconnected"}:
                events.append({"event": kind, "state": "disconnected"})
            else:
                malformed += 1
        elif kind == "FACE" and native_boot(boots[-1] if boots else None):
            values = {key: number(fields, key)
                      for key in ("seq", "expression", "openness", "gaze_x", "gaze_y")}
            limits = {"seq": (0, 65535), "expression": (0, 4), "openness": (0, 16),
                      "gaze_x": (-8, 8), "gaze_y": (-4, 4)}
            if (boots[-1]["firmware"] not in (2, 3)
                    or any(value is None or not limits[key][0] <= value <= limits[key][1]
                           for key, value in values.items())):
                malformed += 1
            else:
                events.append({"event": kind, **values})
    boot = boots[-1] if boots else None
    if boot is not None:
        recognized = native_boot(boot) or (boot["app"] == "ble_display"
                                          and boot["firmware"] == 1 and boot["protocol"] == 1)
        identity = "BOOT" if recognized else "unsupported or malformed BOOT"
    else:
        recognized = any(event["event"] in ("ACK", "SEND") for event in events)
        identity = "ACK/SEND signature after missed boot" if recognized else "none"
    # Unknown/bringup firmware may retain raw logs but cannot supply sender evidence.
    observed = events if recognized else []
    scans = scan_observations if recognized else []

    def selected(kind):
        return [event for event in observed if event["event"] == kind]

    ble, connect, subscribe = selected("BLE"), selected("CONNECT"), selected("SUBSCRIBE")
    acknowledgements, send, disconnect = selected("ACK"), selected("SEND"), selected("DISCONNECT")
    connection_state = None
    for event in observed:
        if event["event"] == "CONNECT":
            connection_state = event["result"] == 0
        elif event["event"] == "DISCONNECT":
            connection_state = False
        elif event["event"] == "LINK" and event["state"] == "disconnected":
            connection_state = False
    return {
        "firmware_recognized": recognized,
        "firmware_boot_observed": bool(boots),
        "boot_records": len(boots),
        "firmware_version": boot["firmware"] if boot else None,
        "firmware_identity_evidence": identity,
        "reported_boot": boot,
        "events": observed,
        "ble_enable_results": [event["enable_result"] for event in ble],
        "ble_enable_succeeded": ble[-1]["enable_result"] == 0 if ble else None,
        "scan_start_results": scan_starts if recognized else [],
        "scan_observations": scans,
        "last_scan_observation": scans[-1] if scans else None,
        "advertisement_callbacks_reported": scans[-1]["callbacks"] if scans else None,
        "connect_results": [event["result"] for event in connect],
        "connect_success_records": sum(event["result"] == 0 for event in connect),
        "connected_at_last_connection_event": connection_state,
        "subscribe_results": [event["result"] for event in subscribe],
        "subscribe_success_records": sum(event["result"] == 0 for event in subscribe),
        "ack_records": acknowledgements,
        "ack_ok_records": sum(event["status"] == 0 for event in acknowledgements),
        "ack_error_or_busy_records": sum(event["status"] != 0 for event in acknowledgements),
        "send_results": [event["result"] for event in send],
        "send_success_records": sum(event["result"] == 0 for event in send),
        "send_error_records": sum(event["result"] != 0 for event in send),
        "face_frames_reported": len(selected("FACE")),
        "face_frames": selected("FACE"),
        "disconnect_reasons": [event["reason"] for event in disconnect],
        "initialization_error_observed": any(event["enable_result"] != 0 for event in ble),
        "malformed_or_incomplete_records": malformed,
        "ack_interpretation": "ACK is a received notification for the pending sequence; "
                              "at_ms is NU87 uptime, not RTT. Counts include repeated sequences.",
        "send_interpretation": "SEND result=0 reports completed GATT writes and an OK ACK; "
                               "it does not verify visible text or animation.",
        "oled_visible": "unverified; human observation required",
        "ble_peer_identity": "unverified by this NU87-only capture",
        "overall_physical_result": "unverified; logs are software and protocol evidence only",
    }


def duration(value):
    try:
        seconds = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("--seconds must be an integer between 20 and 300") from exc
    if not 20 <= seconds <= 300:
        raise argparse.ArgumentTypeError("--seconds must be between 20 and 300")
    return seconds


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-number", required=True, help="exact CP210x USB serial number")
    parser.add_argument("--port", help="explicit device path; USB identity checks still apply")
    parser.add_argument("--seconds", type=duration, default=30)
    parser.add_argument("--confirm-radio-ram", action="store_true",
                        help="send one LF only for the exact v4-radio-preinit "
                             "empty-EFUSE RAM prompt")
    parser.add_argument("--output", type=Path, default=WORKSPACE / "build/nu87-ble.log")
    return parser


def main(argv=None):
    args = make_parser().parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    selected_port = None
    device_buffer = ""
    transcript = []
    handle = None
    capture_error = None
    interrupted = False
    exit_code = 2
    output_bytes = 0
    radio_prompt = RadioRamPrompt()
    serial_write_records = []

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

        def confirm_ram():
            record = {"purpose": "confirm diagnostic RAM warning",
                      "data_hex": "0a", "requested_bytes": 1,
                      "written_bytes": None, "completed": False, "error": None}
            serial_write_records.append(record)
            note("Exact diagnostic RAM prompt observed; sending one LF byte once.")
            try:
                record["written_bytes"] = handle.write(b"\n")
                record["completed"] = record["written_bytes"] == 1
                if not record["completed"]:
                    raise RuntimeError("RAM confirmation did not write exactly one byte.")
            except (Exception, KeyboardInterrupt) as error:
                record["error"] = f"{type(error).__name__}: {error}"
                raise
            note("RAM confirmation LF written; continuing evidence capture.")

        try:
            import serial
            from serial.tools import list_ports

            selected_port = select_port(list_ports.comports(), args.serial_number, args.port)
            note(f"Opening {selected_port} at {BAUD} baud with DTR=False and RTS=False.")
            if args.confirm_radio_ram:
                note("RAM confirmation enabled: one LF only after the exact diagnostic BOOT and "
                     "empty-EFUSE prompt; no reset or firmware write operations.")
            else:
                note("Read-only capture; no serial writes, reset operations or firmware changes.")
            note("Exit 0 means recognized logs were captured; physical output remains unverified.")
            # Construct closed: setting modem lines after opening can reset this NU87.
            serial_options = {"port": None, "baudrate": BAUD, "timeout": 0.1,
                              "rtscts": False, "dsrdtr": False, "xonxoff": False}
            if args.confirm_radio_ram:
                serial_options["write_timeout"] = 1
            handle = serial.Serial(**serial_options)
            handle.dtr = False
            handle.rts = False
            handle.port = selected_port
            handle.open()
            end = time.monotonic() + args.seconds
            while time.monotonic() < end:
                chunk = handle.read(min(max(handle.in_waiting, 1), 4096))
                if not chunk:
                    # Wait for a quiet read so a split suffix cannot turn a similar
                    # unfinished line into the accepted prompt before it is complete.
                    if (args.confirm_radio_ram and not serial_write_records
                            and radio_prompt.feed("")):
                        confirm_ram()
                    continue
                decoded = chunk.decode("utf-8", errors="replace")
                radio_prompt.feed(decoded)
                device_buffer += decoded
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
            if handle is not None:
                try:
                    handle.close()
                except Exception as error:
                    close_error = f"Serial close error: {type(error).__name__}: {error}"
                    if capture_error:
                        capture_error = f"{capture_error}; {close_error}"
                    else:
                        capture_error = close_error
                    note(close_error)
                    if exit_code == 0:
                        exit_code = 2
            if device_buffer:
                emit(device_buffer + "\n")
                device_buffer = ""
            result = summarize("".join(transcript))
            if exit_code == 0 and not result["firmware_recognized"]:
                exit_code = 1
                note("NU87 sender logs not recognized; a USB bridge cannot identify firmware.")
            result.update({
                "port": selected_port,
                "usb_identity": {"vid": "10c4", "pid": "ea60", "serial_number": args.serial_number,
                                 "matched": selected_port is not None},
                "baud": BAUD,
                "dtr": False,
                "rts": False,
                "serial_writes": bool(serial_write_records),
                "serial_write_records": serial_write_records,
                "radio_ram_confirmation": {
                    "enabled": args.confirm_radio_ram,
                    "required_diagnostic": RADIO_DIAGNOSTIC,
                    "qualified_boot_observed": radio_prompt.qualified_boot_observed,
                    "latest_boot_qualified": radio_prompt.firmware_confirmed,
                    "exact_prompt_observed_after_qualified_boot": radio_prompt.prompt_observed,
                    "exact_prompt_observed_without_qualified_boot":
                        radio_prompt.prompt_observed_without_qualified_boot,
                    "exact_prompt_observed_before_qualified_boot":
                        radio_prompt.prompt_observed_before_qualified_boot,
                    "write_attempted": bool(serial_write_records),
                    "confirmation_byte_written": any(record["completed"]
                                                     for record in serial_write_records),
                    "interpretation": "One LF dismisses the verified diagnostic RAM warning; "
                                      "no OTP/EFUSE write command or reset is sent.",
                },
                "reset_requested": False,
                "requested_seconds": args.seconds,
                "capture_completed": exit_code in (0, 1),
                "capture_error": capture_error,
                "interrupted": interrupted,
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
