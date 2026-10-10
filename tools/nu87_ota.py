#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Upload a signed NU40 image over a NU87 USB-to-BLE SMP relay; NU40 needs power only."""

import argparse
import hashlib
import io
import json
import re
import struct
import time
from pathlib import Path

import cbor2

from nu40_ota_package import DEFAULT_KEY, ROOT, validate_signed_image
from nu87_capture import select_port

DEFAULT_SERIAL = "2e060b7be139f11182a1b16f984367d3"
HEADER = struct.Struct(">BBHHBB")
MAX_PACKET = 512


def make_request(op, group, seq, command, values):
    if op not in (0, 2) or not 0 <= group <= 65535 or not 0 <= seq <= 255:
        raise ValueError("Invalid SMP request header")
    data = cbor2.dumps(values)
    packet = HEADER.pack(op, 0, len(data), group, seq, command) + data
    if len(packet) > MAX_PACKET:
        raise ValueError("Request exceeds relay reassembly capacity")
    return packet


def decode_response(request, packet):
    if not 8 <= len(packet) <= MAX_PACKET:
        raise ValueError("Truncated or oversized SMP response")
    req = HEADER.unpack_from(request)
    op, flags, length, group, seq, command = HEADER.unpack_from(packet)
    if ((op, flags, group, seq, command) != (req[0] + 1, 0, req[3], req[4], req[5])
            or length != len(packet) - 8):
        raise ValueError("SMP response does not match the outstanding request")
    stream = io.BytesIO(packet[8:])
    values = cbor2.CBORDecoder(stream).decode()
    if stream.tell() != length or not isinstance(values, dict):
        raise ValueError("SMP payload must be one complete CBOR map")
    remote_error = values.get("err", {})
    if not isinstance(remote_error, dict):
        raise ValueError("Malformed SMP error object")
    if values.get("rc", 0) != 0 or remote_error.get("rc", 0) != 0:
        raise RuntimeError(f"Remote SMP error: {values}")
    return values


def json_safe(value):
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, dict):
        return {key: json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_safe(item) for item in value]
    return value


class Relay:
    def __init__(self, port, logfile):
        self.port, self.logfile, self.sequence = port, logfile, 0

    def line(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.port.readline()
            if not data:
                continue
            line = data.decode("utf-8", errors="replace").strip("\r\n\x00")
            self.logfile.write(line + "\n")
            self.logfile.flush()
            return line
        raise TimeoutError("NU87 relay response timed out")

    def command(self, line):
        encoded = (line + "\n").encode("ascii")
        # Stay below the vendor UART ring buffer while the main task polls.
        # The SDK ring has 127 usable bytes and the paused task polls every 20ms.
        # Two 64-byte writes can fill it before the next poll and lose a byte.
        for offset in range(0, len(encoded), 32):
            self.port.write(encoded[offset:offset + 32])
            self.port.flush()
            time.sleep(0.02)

    def ready(self, timeout=40):
        deadline = time.monotonic() + timeout
        query_at = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= query_at:
                self.command("relay-status")
                query_at = time.monotonic() + 2
            try:
                line = self.line(min(0.5, max(0.1, deadline - time.monotonic())))
            except TimeoutError:
                continue
            match = re.fullmatch(r"NU87\|RELAY firmware=3 smp_ready=([01]) paused=([01])", line)
            if match:
                if match[1] != "1":
                    raise RuntimeError("NU87 connected, but NU40 SMP service is unavailable")
                return
        raise TimeoutError("An OTA relay firmware-3 handshake was not observed")

    def mode(self, value):
        self.command(value)
        # The face task may still be in a GATT wait. Do not stream a large UART
        # request until its polling loop has acknowledged the pause command.
        if value == "pause":
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if self.line(max(0.1, deadline - time.monotonic())) == "NU87|MODE value=pause":
                    return
            raise TimeoutError("NU87 did not acknowledge pausing the face stream")

    def request(self, op, group, command, values):
        self.sequence = (self.sequence + 1) & 255
        request = make_request(op, group, self.sequence, command, values)
        self.command("smp " + request.hex())
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = self.line(max(0.1, deadline - time.monotonic()))
            match = re.fullmatch(r"NU87\|SMP result=(-?\d+) data=([0-9a-f]*)", line)
            if not match:
                continue
            if int(match[1]) != 0:
                raise RuntimeError(f"NU87 SMP relay failed: {match[1]}")
            return decode_response(request, bytes.fromhex(match[2]))
        raise TimeoutError("No matching complete SMP response")


def image_states(reply):
    images = reply.get("images")
    if not isinstance(images, list):
        raise ValueError("Image list missing from remote response")
    result = {}
    for image in images:
        if not isinstance(image, dict) or image.get("image", 0) != 0:
            raise ValueError("Unexpected multi-image state")
        slot = image.get("slot")
        if type(slot) is not int or slot not in (0, 1) or slot in result:
            raise ValueError("Duplicate or invalid image slot")
        if not isinstance(image.get("hash"), bytes) or len(image["hash"]) != 32:
            raise ValueError("Missing image hash")
        if not isinstance(image.get("version"), str):
            raise ValueError("Missing image version")
        result[slot] = image
    if 0 not in result or result[0].get("active") is not True:
        raise ValueError("No active primary image")
    return result


def version_components(value):
    """Accept imgtool's +build and Zephyr image-management's .build notation."""
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:[.+](\d+))?", value)
    if not match:
        raise ValueError("Malformed MCUboot image version")
    result = tuple(int(part or 0) for part in match.groups())
    if any(part > limit for part, limit in zip(result, (255, 255, 65535, 0xffffffff))):
        raise ValueError("MCUboot image version field out of range")
    return result


def upload(relay, image, details, report, progress=print, chunk_size=192):
    states = image_states(relay.request(0, 1, 0, {}))
    report["before"] = json_safe(states)
    wanted_hash = bytes.fromhex(details["image_hash"])
    if states[0]["hash"] == wanted_hash and states[0].get("confirmed") is True:
        report["result"] = "already_installed_and_confirmed"
        return
    if states[0].get("confirmed") is not True:
        raise RuntimeError("Current primary image is still a trial; refusing another update")
    if any(item.get("pending") is True for item in states.values()):
        raise RuntimeError("A previous image is pending; refusing to replace it")
    if not 1 <= chunk_size <= 192:
        raise ValueError("Upload chunk size must be 1..192 bytes")
    relay.mode("pause")
    report["upload_started"] = True
    offset, stalls, highest_percent = 0, 0, -1
    transfer_hash = hashlib.sha256(image).digest()
    while offset < len(image):
        values = {"off": offset, "data": image[offset:offset + chunk_size]}
        if offset == 0:
            values.update(len=len(image), sha=transfer_hash, upgrade=True)
        reply = relay.request(2, 1, 1, values)
        received = reply.get("off")
        if type(received) is not int or not 0 <= received <= len(image):
            raise ValueError("Remote upload offset is out of range")
        if offset != 0 and received > offset + len(values["data"]):
            raise ValueError("Remote upload offset skips unsent data")
        stalls = stalls + 1 if received <= offset else 0
        if stalls >= 3:
            raise RuntimeError("Upload stopped making progress")
        offset = received
        report["uploaded_offset"] = offset
        percent = offset * 100 // len(image)
        if percent // 10 > highest_percent // 10:
            progress(f"[OTA] Upload {percent}% ({offset}/{len(image)} bytes)")
            highest_percent = percent
    states = image_states(relay.request(0, 1, 0, {}))
    secondary = states.get(1, {})
    if (secondary.get("hash") != wanted_hash or
            version_components(secondary.get("version", "")) !=
            version_components(details["version"])):
        raise RuntimeError("Uploaded secondary image does not match the signed target")
    report["secondary_verified"] = json_safe(secondary)
    states = image_states(relay.request(2, 1, 0, {"hash": wanted_hash, "confirm": False}))
    if states.get(1, {}).get("hash") != wanted_hash or states[1].get("pending") is not True:
        raise RuntimeError("Remote image was not scheduled for a trial boot")
    report["trial_scheduled"] = True
    progress("[OTA] Signed target selected for trial boot; requesting one reset.")
    try:
        relay.request(2, 0, 5, {})
        report["reset_response_received"] = True
    except (RuntimeError, TimeoutError) as error:
        # A reset can disconnect before its response; never send another reset here.
        report["reset_response_error"] = str(error)
    relay.mode("face")
    verify_running(relay, details, report, progress)


def verify_running(relay, details, report, progress=print, timeout=90):
    wanted_hash = bytes.fromhex(details["image_hash"])
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            relay.ready(timeout=min(20, max(1, deadline - time.monotonic())))
            states = image_states(relay.request(0, 1, 0, {}))
            report["after"] = json_safe(states)
            if states[0]["hash"] == wanted_hash and states[0].get("confirmed") is True:
                report["result"] = "installed_and_confirmed"
                progress(f"[OTA] NU40 version {states[0]['version']} active and confirmed.")
                return
        except (RuntimeError, TimeoutError) as error:
            report["last_reconnect_error"] = str(error)
        time.sleep(1)
    raise TimeoutError(f"New primary image was not confirmed within {timeout} seconds")


def verify_with_fresh_session(device, logfile, details, report, progress=print):
    """Reopen the relay session for verification only; never repeat a NU40 reset."""
    progress("[OTA] Opening a fresh relay session for read-only image verification.")
    logfile.write("[HOST] Reopening UART session for post-update verification; no NU40 reset.\n")
    logfile.flush()
    device.close()
    device.open()
    report["verification_session_reopened"] = True
    verify_running(Relay(device, logfile), details, report, progress, timeout=45)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-number", default=DEFAULT_SERIAL)
    parser.add_argument("--port")
    parser.add_argument("--image", type=Path)
    parser.add_argument("--key", type=Path, default=DEFAULT_KEY)
    parser.add_argument("--write", action="store_true", help="upload, trial-boot and verify image")
    parser.add_argument("--output", type=Path, default=ROOT / "build/nu87-ota")
    args = parser.parse_args(argv)
    if args.write != (args.image is not None):
        parser.error("An update requires both --image and --write; omit both for image-list only")
    details = validate_signed_image(args.image, args.key) if args.image else None
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"serial_number": args.serial_number, "write_requested": args.write,
              "target": details, "result": "not_started", "oled_visible": "manual observation"}
    selected = None
    try:
        import serial
        from serial.tools import list_ports

        selected = select_port(list_ports.comports(), args.serial_number, args.port)
        device = serial.Serial(port=None, baudrate=1500000, timeout=0.1, write_timeout=3)
        device.dtr = False
        device.rts = False
        device.port = selected
        with (args.output / "relay.log").open("w", encoding="utf-8", buffering=1) as log:
            with device:
                relay = Relay(device, log)
                relay.ready()
                print("[OTA] NU87 relay firmware 3 and remote SMP service confirmed.", flush=True)
                if args.write:
                    try:
                        upload(relay, args.image.read_bytes(), details, report,
                               progress=lambda value: print(value, flush=True))
                    except TimeoutError as error:
                        if not report.get("trial_scheduled"):
                            raise
                        report["verification_first_session_error"] = str(error)
                        verify_with_fresh_session(device, log, details, report,
                                                  progress=lambda value: print(value, flush=True))
                    finally:
                        try:
                            relay.mode("face")
                        except (OSError, RuntimeError) as error:
                            report["resume_face_error"] = str(error)
                else:
                    report["images"] = json_safe(image_states(relay.request(0, 1, 0, {})))
                    report["result"] = "image_list_received"
        return 0
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        report["result"] = "failed_or_incomplete"
        print("[OTA] " + report["error"], flush=True)
        return 2
    finally:
        report["port"] = selected
        (args.output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)
                                               + "\n", encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    raise SystemExit(main())
