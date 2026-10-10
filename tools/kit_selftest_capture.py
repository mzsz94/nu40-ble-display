#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture NU40DK-KIT selftest evidence without treating logs as physical verification."""

import argparse
import json
import re
import statistics
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[1]
DEFAULT_PRODUCT = "NU40DK KIT selftest"
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
FIELD = re.compile(r"\b([a-z_]+)=([^\s]+)")


def select_port(ports, product=DEFAULT_PRODUCT, serial_number=None):
    """Prefer USB identity; never silently ignore an explicitly requested serial number."""
    matches = [
        port for port in ports
        if (not product or product.casefold() in (port.product or "").casefold())
        and (serial_number is None or port.serial_number == serial_number)
    ]
    if len(matches) == 1:
        return matches[0].device, "USB product/serial match"
    if len(matches) > 1:
        raise ValueError("Multiple matching USB ports; specify --serial-number or --port.")
    if serial_number is not None or product != DEFAULT_PRODUCT:
        raise ValueError("No USB port matches the requested product/serial filters.")
    return "/dev/ttyACM0", "fallback; firmware identity still requires KIT selftest logs"


def _number(fields, name):
    try:
        value = fields[name]
        return int(value, 16 if value.lower().startswith("0x") else 10)
    except (KeyError, ValueError):
        return None


def _mic_comparison(samples, stream_errors):
    phases = {}
    phase_samples = {}
    for phase in ("silence", "voice"):
        # Repeated summaries of the same audio block are one sample, not independent evidence.
        selected = list({
            sample["blocks"]: sample for sample in samples if sample["phase"] == phase
        }.values())
        phase_samples[phase] = selected
        phases[phase] = {
            "samples": len(selected),
            "median_rms": statistics.median([sample["rms"] for sample in selected])
            if selected else None,
            "median_peak": statistics.median([sample["peak"] for sample in selected])
            if selected else None,
            "max_rms": max(sample["rms"] for sample in selected) if selected else None,
            "max_peak": max(sample["peak"] for sample in selected) if selected else None,
        }
    blocks = [sample["blocks"] for sample in samples]
    advancing = (len(blocks) >= 2 and blocks[-1] > blocks[0]
                 and all(second >= first
                         for first, second in zip(blocks, blocks[1:], strict=False)))
    clipping = sum(sample["peak"] >= 32700 for sample in samples)
    silence, voice = phases["silence"], phases["voice"]
    thresholds = None
    elevated_windows = None
    if silence["samples"] >= 2:
        thresholds = {
            "rms": max(silence["median_rms"] * 2, silence["median_rms"] + 100),
            "peak": max(silence["median_peak"] * 2, silence["median_peak"] + 256),
        }
        elevated_windows = sum(
            sample["rms"] >= thresholds["rms"] and sample["peak"] >= thresholds["peak"]
            for sample in phase_samples["voice"]
        )
    if stream_errors:
        response = "stream_error_observed"
    elif silence["samples"] < 2 or voice["samples"] < 3:
        response = "insufficient_phase_samples"
    elif not advancing:
        response = "blocks_not_advancing"
    else:
        rms_rise = voice["median_rms"] >= thresholds["rms"]
        peak_rise = voice["median_peak"] >= thresholds["peak"]
        response = (
            "level_increase_observed" if rms_rise and peak_rise else "no_clear_level_increase"
        )
    return {
        "phase_statistics": phases,
        "blocks_advanced": advancing,
        "near_full_scale_samples": clipping,
        "response_evidence": response,
        "voice_windows_with_level_increase": elevated_windows,
        "level_increase_thresholds": thresholds,
        "interpretation": (
            "Level comparison only; noise, DC offset or electrical interference can also change "
            "peak/RMS. This does not verify acoustic capture, speech quality or recognition."
        ),
    }


def _sd_evidence(records, recognized):
    """Keep SD presence, initialization and one-sector reading as separate evidence."""
    observed = records if recognized else []

    def latest(name):
        values = [_number(fields, name) for fields in observed]
        values = [value for value in values if value is not None]
        return values[-1] if values else None

    detection = []
    for fields in observed:
        value = _number(fields, "detect_result" if "detect_result" in fields else "present")
        if value is not None:
            detection.append(value)
    detect_result = detection[-1] if detection else None
    present = None if detect_result is None or detect_result < 0 else detect_result > 0
    read_result = latest("read_result")
    # Target Zephyr/Picolibc uses EINPROGRESS=119; retain Linux-style fixture compatibility.
    pending = read_result in (-119, -115)
    if read_result is None or pending:
        read_succeeded = None
    else:
        read_succeeded = read_result == 0
    sectors, sector_bytes = latest("sectors"), latest("sector_bytes")
    capacity = sectors * sector_bytes if sectors and sector_bytes else None
    readonly = latest("readonly")
    return {
        "evidence_observed": bool(observed),
        "detect_result": detect_result,
        "card_present": present,
        "initialization_result": latest("init_result"),
        "read_result": read_result,
        "read_in_progress": pending if read_result is not None else None,
        "sector0_read_succeeded": read_succeeded,
        "sector_count_reported": sectors,
        "sector_bytes_reported": sector_bytes,
        "capacity_bytes_reported": capacity,
        "read_lba_reported": latest("lba"),
        "read_sector_count_reported": latest("count"),
        "read_only_reported": readonly == 1 if readonly is not None else None,
        "filesystem": "not tested; no mount or file operations",
        "whole_card_integrity": "not verified; at most sector 0 was read",
        "writes_format_erase": "not tested; firmware performs read-only diagnostics",
    }


def summarize(text):
    """Report recorded firmware events separately from human sensory observations."""
    phase = "unlabelled"
    records = []
    for line in ANSI_ESCAPE.sub("", text).replace("\r", "").splitlines():
        marker = re.fullmatch(r"\[HOST\] PHASE (\w+)", line)
        if marker:
            phase = marker[1]
        match = re.match(r"KIT\|([A-Z]+)\s+(.*)", line)
        if match:
            # Concurrent printk calls can overwrite a line's tail with another KIT record.
            # Keep only this record's surviving prefix; never fill in missing SD geometry.
            body = match[2].split("KIT|", 1)[0]
            records.append((match[1], dict(FIELD.findall(body)), phase, body))
    versions = [
        _number(fields, "firmware") for kind, fields, _, body in records
        if kind == "BOOT" and body.startswith("NU40DK-KIT selftest ")
    ]
    versions = [version for version in versions if version is not None]
    firmware_version = versions[-1] if versions else None
    boot = firmware_version in (1, 2, 3)
    status_records = [fields for kind, fields, _, _ in records if kind == "STATUS"]
    mic_samples = []
    for kind, fields, sample_phase, _ in records:
        values = [_number(fields, name) for name in ("peak", "rms", "blocks")]
        if (kind == "MIC" and all(value is not None and value >= 0 for value in values)
                and _number(fields, "rate") == 16000 and _number(fields, "width") == 16
                and _number(fields, "channels") == 1):
            mic_samples.append(dict(zip(("peak", "rms", "blocks"), values, strict=True)))
            mic_samples[-1]["phase"] = sample_phase
    # A collector may attach after BOOT. Require both distinctive periodic records then.
    recognized = boot or (firmware_version is None and bool(mic_samples) and any(
        all(name in fields for name in ("keys_ready", "oled_write_ok", "mic_errors", "beep_tx_ok"))
        for fields in status_records
    ))

    def last_result(kind, name):
        values = [_number(fields, name) for item, fields, _, _ in records if item == kind]
        values = [value for value in values if value is not None]
        return values[-1] if recognized and values else None

    keys = set()
    if recognized:
        for kind, fields, _, _ in records:
            mask = _number(fields, "pressed") if kind == "BUTTON" else None
            if mask is not None:
                keys.update(index for index in range(4) if mask & (1 << index))
    mic_errors = [
        value for fields in status_records
        if (value := _number(fields, "mic_errors")) is not None
    ]
    stream_error = any(mic_errors) or any(
        kind == "MIC" and "read_error" in fields for kind, fields, _, _ in records
    )
    oled_error = any(kind == "OLED" and "write_error" in fields
                     for kind, fields, _, _ in records)
    rgb_error = any(kind == "RGB" and "write_error" in fields
                    for kind, fields, _, _ in records)
    oled_writes = [_number(fields, "oled_write_ok") for fields in status_records]
    oled_writes = [value for value in oled_writes if value is not None]
    tx_result = last_result("SPEAKER", "tx_result")
    tx_counts = [_number(fields, "beep_tx_ok") for fields in status_records]
    tx_counts = [value for value in tx_counts if value is not None]
    transfer = tx_result == 0 if tx_result is not None else None
    count_advanced = recognized and any(
        second > first for first, second in zip(tx_counts, tx_counts[1:], strict=False)
    )
    attempted = None
    if tx_result is not None or count_advanced:
        attempted = True
    elif recognized and tx_counts and not any(tx_counts):
        attempted = False
    if transfer is None and count_advanced:
        transfer = True
    result = {
        "selftest_firmware_recognized": recognized,
        "firmware_boot_observed": boot,
        "firmware_version": firmware_version,
        "keys_pressed_during_capture": sorted(keys),
        "all_four_key_presses_observed": recognized and len(keys) == 4,
        "oled_init_result": last_result("OLED", "init_result"),
        "oled_write_count_reported": max(oled_writes) if recognized and oled_writes else None,
        "oled_write_error_observed": recognized and oled_error,
        "oled_visible": "requires visual observation",
        "speaker_init_result": last_result("SPEAKER", "init_result"),
        "speaker_test_attempted": attempted,
        "speaker_transfer_succeeded": transfer,
        "speaker_audible": "requires listening",
        "rgb_driver_ready": last_result("RGB", "driver_ready"),
        "rgb_write_error_observed": recognized and rgb_error,
        "rgb_visible": "requires visual observation",
        "microphone_start_result": last_result("MIC", "start_result"),
        "microphone_read_error_observed": recognized and stream_error,
        "microphone": _mic_comparison(mic_samples if recognized else [], stream_error),
        "microphone_acoustic_capture": "unverified; phase level comparison is evidence only",
        "microsd": _sd_evidence(
            [fields for kind, fields, _, _ in records if kind == "SD"], recognized,
        ),
        "overall_physical_result": "unverified; no automatic hardware PASS",
    }
    if not recognized:
        result["microphone"]["response_evidence"] = "selftest_firmware_not_recognized"
    return result


def duration(value):
    number = float(value)
    if not 20 <= number <= 300:
        raise argparse.ArgumentTypeError("seconds must be between 20 and 300")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Explicit serial device, bypassing USB identity selection")
    parser.add_argument("--product", default=DEFAULT_PRODUCT, help="USB product substring")
    parser.add_argument("--serial-number", help="Exact USB serial number")
    parser.add_argument("--seconds", type=duration, default=60)
    parser.add_argument("--output", type=Path, default=WORKSPACE / "build/kit-selftest.log")
    args = parser.parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    transcript = []
    capture_error = None
    interrupted = False
    exit_code = 2
    selected_port = args.port
    device_buffer = ""

    with args.output.open("w", encoding="utf-8", buffering=1) as logfile:
        def emit(text):
            transcript.append(text)
            logfile.write(text)
            print(text, end="", flush=True)

        def note(message):
            nonlocal device_buffer
            if device_buffer:
                emit(device_buffer + "\n")
                device_buffer = ""
            emit(f"[HOST] {message}\n")

        def collect(port, seconds):
            nonlocal device_buffer
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                chunk = port.read(min(max(port.in_waiting, 1), 4096))
                if chunk:
                    device_buffer += chunk.decode("utf-8", errors="replace")
                    while "\n" in device_buffer:
                        line, device_buffer = device_buffer.split("\n", 1)
                        emit(line + "\n")
                    if sum(map(len, transcript)) + len(device_buffer) > 2 * 1024 * 1024:
                        raise RuntimeError("Stopped after 2 MiB of device output.")

        try:
            import serial
            from serial.tools import list_ports

            if selected_port:
                selection = "explicit --port"
            else:
                selected_port, selection = select_port(
                    list_ports.comports(), args.product, args.serial_number,
                )
            note(f"Opening {selected_port} at 115200 baud ({selection}).")
            note("Read-only capture; no serial commands or firmware changes.")
            note("Exit 0 means capture completed with selftest records; it is not a hardware PASS.")
            with serial.Serial(selected_port, 115200, timeout=0.1) as port:
                key_seconds = min(25.0, args.seconds - 15.0)
                note("PHASE keys")
                note("KIT의 큰 버튼 4개를 차례로 눌렀다 놓으세요. 작은 보드 버튼과 구분하세요.")
                note("OLED 내용·RGB 변화를 눈으로, 세 번째 버튼의 작은 비프음을 귀로 확인하세요.")
                collect(port, key_seconds)
                note("PHASE silence")
                note("버튼을 놓고 5초 동안 조용히 기다리세요. 마이크 기준 구간입니다.")
                collect(port, 5)
                note("PHASE voice")
                note("이제 마이크 가까이에서 10초 동안 말하거나 손뼉을 반복하세요.")
                collect(port, 10)
                remaining = args.seconds - key_seconds - 15
                if remaining > 0:
                    note("PHASE observe")
                    note("남은 시간에는 버튼·OLED·RGB·비프음을 추가 확인하세요.")
                    collect(port, remaining)
            exit_code = 0
        except KeyboardInterrupt:
            interrupted = True
            capture_error = "Interrupted by user; partial evidence retained."
            exit_code = 130
            note(capture_error)
        except Exception as error:
            # Persist partial evidence for missing pyserial, USB removal and read failures alike.
            capture_error = f"{type(error).__name__}: {error}"
            note(f"Capture error: {capture_error}")
            if isinstance(error, ImportError):
                note("Zephyr 가상환경의 Python(pyserial 설치됨)으로 실행하세요.")
        finally:
            if device_buffer:
                emit(device_buffer + "\n")
                device_buffer = ""
            result = summarize("".join(transcript))
            result.update({
                "port": selected_port,
                "requested_seconds": args.seconds,
                "capture_completed": exit_code == 0,
                "interrupted": interrupted,
                "capture_error": capture_error,
            })
            if exit_code == 0 and not result["selftest_firmware_recognized"]:
                exit_code = 1
                note("KIT selftest firmware not recognized; IMU logs are not KIT evidence.")
            result["exit_code"] = exit_code
            result_path = args.output.with_suffix(".json")
            result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
            note(f"Log saved: {args.output}")
            note(f"Evidence summary: {result_path}")
            print(json.dumps(result, ensure_ascii=False, indent=2))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
