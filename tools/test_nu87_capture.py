# SPDX-License-Identifier: Apache-2.0
"""Check NU87 evidence and closed-port modem-line setup without accessing devices."""

import argparse
import contextlib
import io
import itertools
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import nu87_capture as capture

SERIAL = "nu87-test-bridge"
DEVICE = "/dev/ttyUSB7"
BOOT = "NU87|BOOT app=ble_display firmware=1 protocol=1 baud=1500000\n"
ACK = "NU87|ACK seq=7 status=0 at_ms=1234\n"
SEND = "NU87|SEND result=0\n"
RADIO_BOOT = BOOT.rstrip("\n") + " diagnostic=v4-radio-preinit\n"
RADIO_PROMPT = ("Efuse empty! Wifi performance may be affected. "
                "Press any key to ignore and continue")
SCAN = ("NU87|SCAN waiting=1 uptime_ms=7900 callbacks=0 connectable=0 uuid_match=0 "
        "candidate_queued=0 candidate_dropped=0 connected=0 ready=0\n")


def target(**changes):
    fields = {"device": DEVICE, "vid": 0x10C4, "pid": 0xEA60, "serial_number": SERIAL}
    fields.update(changes)
    return SimpleNamespace(**fields)


class IdentityTests(unittest.TestCase):
    def test_exact_unique_bridge_identity_is_required_even_with_explicit_path(self):
        self.assertEqual(capture.select_port([target()], SERIAL), DEVICE)
        self.assertEqual(capture.select_port([target()], SERIAL, DEVICE), DEVICE)
        variants = ({"vid": 0x2FE3}, {"pid": 4}, {"serial_number": "other"},
                    {"vid": None}, {"pid": None}, {"serial_number": None})
        for fields in variants:
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                capture.select_port([target(**fields)], SERIAL, DEVICE)

    def test_ambiguous_serial_cannot_be_disambiguated_by_port_argument(self):
        ports = [target(), target(device="/dev/ttyUSB8")]
        for port in (None, DEVICE, "/dev/ttyUSB8"):
            with self.subTest(port=port), self.assertRaises(ValueError):
                capture.select_port(ports, SERIAL, port)

    def test_missing_serial_device_or_wrong_explicit_path_is_rejected(self):
        for ports, serial, port in (([], SERIAL, None), ([target()], "", None),
                                    ([target()], "   ", DEVICE), ([target()], None, DEVICE),
                                    ([target()], SERIAL, "/dev/ttyUSB0")):
            with self.subTest(serial=serial, port=port), self.assertRaises(ValueError):
                capture.select_port(ports, serial, port)

    def test_unrelated_bridge_does_not_change_exact_selection(self):
        ports = [target(serial_number="another-bridge", device="/dev/ttyUSB0"), target()]
        self.assertEqual(capture.select_port(ports, SERIAL), DEVICE)

    def test_cli_requires_serial_and_defaults_to_thirty_seconds(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            capture.make_parser().parse_args([])
        self.assertEqual(error.exception.code, 2)
        args = capture.make_parser().parse_args(["--serial-number", SERIAL])
        self.assertEqual(args.seconds, 30)
        self.assertIsNone(args.port)
        self.assertFalse(args.confirm_radio_ram)

    def test_duration_bounds_and_nonintegers(self):
        self.assertEqual(capture.duration("20"), 20)
        self.assertEqual(capture.duration("300"), 300)
        for value in ("19", "301", "-30", "30.5", "oops", ""):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                capture.duration(value)


class ParserTests(unittest.TestCase):
    def test_native_ota_relay_v3_recognizes_face_and_ack_evidence(self):
        boot = ("NU87|BOOT app=ble_display_native firmware=3 protocol=1 "
                "baud=1500000 diagnostic=arduino-3.1.9 demo=face features=smp_relay\n")
        frame = "NU87|FACE seq=7 expression=4 openness=16 gaze_x=-8 gaze_y=4\n"
        result = capture.summarize(boot + frame + ACK + SEND)
        self.assertEqual(result["firmware_version"], 3)
        self.assertEqual(result["face_frames_reported"], 1)
        self.assertEqual(result["ack_ok_records"], 1)

    def test_face_firmware_v2_records_valid_frames_with_ack_without_visual_claim(self):
        boot = ("NU87|BOOT app=ble_display_native firmware=2 protocol=1 "
                "baud=1500000 diagnostic=arduino-3.1.9 demo=face\n")
        frame = "NU87|FACE seq=7 expression=4 openness=16 gaze_x=-8 gaze_y=4\n"
        result = capture.summarize(boot + frame + ACK + SEND)
        self.assertTrue(result["firmware_recognized"])
        self.assertEqual(result["firmware_version"], 2)
        self.assertEqual(result["face_frames_reported"], 1)
        self.assertEqual(result["ack_ok_records"], 1)
        self.assertEqual(result["face_frames"][0]["expression"], 4)
        self.assertIn("unverified", result["oled_visible"])
        self.assertEqual(capture.summarize(boot + frame + boot)["face_frames_reported"], 0)
        for malformed in (frame.replace("expression=4", "expression=5"),
                          frame.replace("openness=16", "openness=17"),
                          frame.replace("gaze_x=-8", "gaze_x=-9"),
                          frame.replace("gaze_y=4", "gaze_y=5"),
                          frame.replace("seq=7", "seq=65536")):
            with self.subTest(frame=malformed):
                result = capture.summarize(boot + malformed)
                self.assertEqual(result["face_frames_reported"], 0)
                self.assertEqual(result["malformed_or_incomplete_records"], 1)

    def test_reset_nul_padding_before_boot_does_not_hide_native_scan_evidence(self):
        boot = ("NU87|BOOT app=ble_display_native firmware=1 protocol=1 "
                "baud=1500000 diagnostic=arduino-3.1.9\n")
        evidence = "NU87|BLE enable_result=0\n" + SCAN.replace("callbacks=0", "callbacks=1109")
        result = capture.summarize("\x00" * 128 + boot + evidence)
        self.assertTrue(result["firmware_recognized"])
        self.assertTrue(result["firmware_boot_observed"])
        self.assertEqual(result["ble_enable_results"], [0])
        self.assertEqual(result["advertisement_callbacks_reported"], 1109)
        self.assertFalse(capture.summarize(
            boot.replace("arduino-3.1.9", "arduino-3.1\x00.9") + evidence
        )["firmware_recognized"])

    def test_native_disconnect_changes_state_without_inventing_a_reason(self):
        boot = ("NU87|BOOT app=ble_display_native firmware=1 protocol=1 "
                "baud=1500000 diagnostic=arduino-3.1.9\n")
        link = "NU87|LINK state=disconnected\n"
        result = capture.summarize(boot + "NU87|CONNECT result=0\n" + link)
        self.assertFalse(result["connected_at_last_connection_event"])
        self.assertEqual(result["disconnect_reasons"], [])
        result = capture.summarize(BOOT + "NU87|CONNECT result=0\n" + link)
        self.assertTrue(result["connected_at_last_connection_event"])

    def test_native_signed_connection_errors_require_its_exact_boot(self):
        boot = ("NU87|BOOT app=ble_display_native firmware=1 protocol=1 "
                "baud=1500000 diagnostic=arduino-3.1.9\n")
        errors = "NU87|CONNECT result=-2\nNU87|SUBSCRIBE result=-6\n"
        result = capture.summarize(boot + errors)
        self.assertEqual(result["connect_results"], [-2])
        self.assertEqual(result["subscribe_results"], [-6])
        self.assertEqual(result["connect_success_records"], 0)
        self.assertEqual(result["subscribe_success_records"], 0)
        result = capture.summarize(BOOT + errors)
        self.assertEqual(result["events"], [])
        self.assertEqual(result["malformed_or_incomplete_records"], 2)

    def test_native_sender_requires_exact_app_diagnostic_and_baud(self):
        boot = BOOT.replace("app=ble_display", "app=ble_display_native").rstrip("\n")
        boot += " diagnostic=arduino-3.1.9\n"
        result = capture.summarize(boot + ACK + SEND)
        self.assertTrue(result["firmware_recognized"])
        self.assertEqual(result["reported_boot"]["app"], "ble_display_native")
        self.assertEqual(result["send_success_records"], 1)
        self.assertEqual(result["ack_ok_records"], 1)
        for altered in (boot.replace("arduino-3.1.9", "unknown"),
                        boot.replace(" diagnostic=arduino-3.1.9", ""),
                        boot.replace("1500000", "115200")):
            with self.subTest(boot=altered):
                result = capture.summarize(altered + ACK + SEND)
                self.assertFalse(result["firmware_recognized"])
                self.assertEqual(result["events"], [])

    def test_scan_start_success_and_zero_callbacks_do_not_imply_a_link(self):
        result = capture.summarize(BOOT + "NU87|SCAN start_result=0\n" + SCAN)
        self.assertEqual(result["scan_start_results"], [0])
        self.assertEqual(result["advertisement_callbacks_reported"], 0)
        self.assertEqual(result["last_scan_observation"]["uptime_ms"], 7900)
        self.assertEqual(result["send_success_records"], 0)
        self.assertEqual(result["ack_ok_records"], 0)
        self.assertIsNone(result["connected_at_last_connection_event"])
        self.assertIsNone(capture.summarize(BOOT)["advertisement_callbacks_reported"])

    def test_scan_reports_preserve_observations_and_latest_counter_snapshot(self):
        later = SCAN.replace("callbacks=0", "callbacks=12").replace("uuid_match=0", "uuid_match=2")
        result = capture.summarize(BOOT + SCAN + later)
        self.assertEqual(len(result["scan_observations"]), 2)
        self.assertEqual(result["advertisement_callbacks_reported"], 12)
        self.assertEqual(result["last_scan_observation"]["uuid_match"], 2)
        self.assertEqual(result["connect_success_records"], 0)

    def test_scan_records_reset_on_reboot_and_require_recognized_firmware(self):
        old = BOOT + "NU87|SCAN start_result=0\n" + SCAN
        for text in (old + BOOT, SCAN, BOOT.replace("app=ble_display", "app=bringup") + SCAN):
            with self.subTest(text=text):
                result = capture.summarize(text)
                self.assertEqual(result["scan_observations"], [])
                self.assertEqual(result["scan_start_results"], [])
                self.assertIsNone(result["last_scan_observation"])

    def test_partial_invalid_and_interleaved_scan_records_are_not_combined(self):
        variants = (SCAN.replace(" ready=0", ""), SCAN.replace("callbacks=0", "callbacks=-1"),
                    SCAN.replace("waiting=1", "waiting=2"),
                    SCAN.replace("uptime_ms=7900", "uptime_ms=4294967296"),
                    SCAN.replace(" ready=0", " NU87|SCAN ready=0"),
                    SCAN.replace("callbacks=0", "callbacks=0 callbacks=1"),
                    "NU87|SCAN start_result=bad\n")
        for text in variants:
            with self.subTest(text=text):
                result = capture.summarize(BOOT + text)
                self.assertEqual(result["scan_observations"], [])
                self.assertEqual(result["scan_start_results"], [])
                self.assertEqual(result["malformed_or_incomplete_records"], 1)

    def test_boot_identity_does_not_imply_connection_send_or_visible_success(self):
        result = capture.summarize(BOOT)
        self.assertTrue(result["firmware_recognized"])
        self.assertTrue(result["firmware_boot_observed"])
        self.assertEqual(result["firmware_version"], 1)
        self.assertEqual(result["reported_boot"]["baud"], 1500000)
        self.assertEqual(result["firmware_identity_evidence"], "BOOT")
        self.assertEqual(result["send_success_records"], 0)
        self.assertEqual(result["ack_ok_records"], 0)
        self.assertIsNone(result["connected_at_last_connection_event"])
        self.assertIn("unverified", result["oled_visible"])
        self.assertIn("unverified", result["ble_peer_identity"])
        self.assertIn("unverified", result["overall_physical_result"])

    def test_after_missed_boot_only_ack_or_send_signatures_recognize_sender(self):
        for text in (ACK, SEND, ACK + SEND):
            with self.subTest(text=text):
                result = capture.summarize(text)
                self.assertTrue(result["firmware_recognized"])
                self.assertFalse(result["firmware_boot_observed"])
                self.assertIsNone(result["firmware_version"])
        text = ("NU87|BLE enable_result=0\nNU87|CONNECT result=0\n"
                "NU87|SUBSCRIBE result=0\nNU87|DISCONNECT reason=19\n")
        self.assertFalse(capture.summarize(text)["firmware_recognized"])

    def test_explicit_wrong_boot_cannot_fallback_to_valid_ack_or_send(self):
        for boot in (BOOT.replace("app=ble_display", "app=bringup"),
                     BOOT.replace("firmware=1", "firmware=10"),
                     BOOT.replace("protocol=1", "protocol=2"),
                     BOOT.replace("firmware=1 ", "")):
            with self.subTest(boot=boot):
                result = capture.summarize(boot + ACK + SEND)
                self.assertFalse(result["firmware_recognized"])
                self.assertEqual(result["events"], [])
                self.assertEqual(result["send_success_records"], 0)
                self.assertEqual(result["ack_ok_records"], 0)

    def test_latest_boot_resets_evidence_from_previous_firmware_session(self):
        old_session = BOOT + ACK + SEND + "NU87|CONNECT result=0\n"
        result = capture.summarize(old_session + BOOT)
        self.assertTrue(result["firmware_recognized"])
        self.assertEqual(result["events"], [])
        self.assertEqual(result["ack_ok_records"], 0)
        self.assertEqual(result["send_success_records"], 0)
        self.assertIsNone(result["connected_at_last_connection_event"])
        unknown_boot = BOOT.replace("app=ble_display", "app=bringup")
        result = capture.summarize(unknown_boot + ACK + SEND + BOOT + SEND)
        self.assertEqual(result["ack_ok_records"], 0)
        self.assertEqual(result["send_success_records"], 1)

    def test_bringup_usb_text_and_other_firmware_do_not_recognize_sender(self):
        for text in ("NU87 bringup heartbeat=1\n", "CP2102N USB to UART Bridge\n",
                     "NU87|BOOT app=bringup firmware=1\n",
                     "LINK|RX seq=7 bytes=5 oled_result=0 ack_result=0 visible=unverified\n",
                     "[HOST] NU87|SEND result=0\n"):
            with self.subTest(text=text):
                self.assertFalse(capture.summarize(text)["firmware_recognized"])

    def test_all_events_keep_init_connection_ack_and_send_results_separate(self):
        text = BOOT + "NU87|BLE enable_result=-5\nNU87|CONNECT result=13\n"
        text += "NU87|CONNECT result=0\nNU87|SUBSCRIBE result=5\nNU87|SUBSCRIBE result=0\n"
        for status in range(4):
            text += ACK.replace("status=0", f"status={status}")
        text += SEND + "NU87|SEND result=-110\nNU87|DISCONNECT reason=19\n"
        result = capture.summarize(text)
        self.assertEqual(result["ble_enable_results"], [-5])
        self.assertFalse(result["ble_enable_succeeded"])
        self.assertTrue(result["initialization_error_observed"])
        self.assertEqual(result["connect_results"], [13, 0])
        self.assertEqual(result["connect_success_records"], 1)
        self.assertEqual(result["subscribe_results"], [5, 0])
        self.assertEqual(result["subscribe_success_records"], 1)
        self.assertEqual([ack["status"] for ack in result["ack_records"]], [0, 1, 2, 3])
        self.assertEqual(result["ack_ok_records"], 1)
        self.assertEqual(result["ack_error_or_busy_records"], 3)
        self.assertEqual(result["send_results"], [0, -110])
        self.assertEqual(result["send_success_records"], 1)
        self.assertEqual(result["send_error_records"], 1)
        self.assertEqual(result["disconnect_reasons"], [19])
        self.assertFalse(result["connected_at_last_connection_event"])

    def test_last_connection_event_and_ble_enable_result_follow_event_order(self):
        result = capture.summarize(BOOT + "NU87|BLE enable_result=-5\n"
                                   "NU87|BLE enable_result=0\nNU87|CONNECT result=0\n"
                                   "NU87|DISCONNECT reason=19\nNU87|CONNECT result=0\n")
        self.assertTrue(result["ble_enable_succeeded"])
        self.assertTrue(result["initialization_error_observed"])
        self.assertTrue(result["connected_at_last_connection_event"])

    def test_duplicate_ack_records_are_not_reported_as_unique_messages(self):
        result = capture.summarize(BOOT + ACK + ACK + SEND)
        self.assertEqual(result["ack_ok_records"], 2)
        self.assertEqual(result["send_success_records"], 1)
        self.assertEqual(result["ack_records"][0]["at_ms"], 1234)
        self.assertIn("received notification", result["ack_interpretation"])
        self.assertIn("not RTT", result["ack_interpretation"])
        self.assertIn("does not verify visible", result["send_interpretation"])

    def test_numeric_boundaries_and_ansi_wrappers_are_accepted(self):
        text = "\x1b[32m" + BOOT + "\x1b[0m"
        text += "NU87|ACK seq=65535 status=3 at_ms=4294967295\r\n"
        text += "NU87|ACK seq=0 status=0 at_ms=0\n"
        text += "NU87|SEND result=-2147483648\nNU87|SEND result=2147483647\n"
        text += "NU87|CONNECT result=255\nNU87|SUBSCRIBE result=255\n"
        text += "NU87|DISCONNECT reason=255\n"
        result = capture.summarize(text)
        self.assertEqual(result["malformed_or_incomplete_records"], 0)
        self.assertEqual(result["ack_records"][0]["at_ms"], 0xFFFFFFFF)
        self.assertEqual(result["ack_records"][1]["seq"], 0)
        self.assertEqual(result["send_results"], [-2147483648, 2147483647])

    def test_malformed_or_out_of_range_events_do_not_supply_evidence(self):
        variants = ("NU87|ACK seq=7 status=0\n", "NU87|ACK seq=7 status=0 at_ms=bad\n",
                    ACK.replace("seq=7", "seq=-1"), ACK.replace("seq=7", "seq=65536"),
                    ACK.replace("status=0", "status=4"),
                    ACK.replace("at_ms=1234", "at_ms=4294967296"),
                    ACK.replace("seq=7", "seq=7 seq=8"),
                    SEND.replace("result=0", "result=0garbage"),
                    SEND.replace("result=0", "result=2147483648"),
                    "NU87|CONNECT result=-1\n", "NU87|SUBSCRIBE result=256\n",
                    "NU87|DISCONNECT reason=-1\n", "NU87|BLE enable_result=oops\n")
        for text in variants:
            with self.subTest(text=text):
                result = capture.summarize(text)
                self.assertFalse(result["firmware_recognized"])
                self.assertEqual(result["events"], [])
                self.assertEqual(result["malformed_or_incomplete_records"], 1)

    def test_interleaved_partial_tail_cannot_supply_missing_ack_fields(self):
        text = "NU87|ACK seq=7 status=0 at_msNU87|SEND result=0\n"
        result = capture.summarize(text)
        self.assertFalse(result["firmware_recognized"])
        self.assertEqual(result["ack_ok_records"], 0)
        self.assertEqual(result["malformed_or_incomplete_records"], 1)


class RadioPromptTests(unittest.TestCase):
    def test_native_boot_does_not_authorize_zephyr_ram_prompt_confirmation(self):
        native_boot = RADIO_BOOT.replace("app=ble_display", "app=ble_display_native")
        native_boot = native_boot.replace("v4-radio-preinit", "arduino-3.1.9")
        observer = capture.RadioRamPrompt()
        self.assertFalse(observer.feed(native_boot + "[WLAN-E] " + RADIO_PROMPT))
        self.assertFalse(observer.qualified_boot_observed)

    def test_native_wifi_prefix_is_accepted_across_chunks_and_sentence_wrap(self):
        prompts = ("[WLAN-E] " + RADIO_PROMPT,
                   "[WLAN-E] " + RADIO_PROMPT.replace(". Press", ".\r\nPress"),
                   "[WLAN-E] " + RADIO_PROMPT.replace(". Press", ".\n[WLAN-E] Press"))
        for prompt in prompts:
            with self.subTest(prompt=prompt):
                observer = capture.RadioRamPrompt()
                for character in RADIO_BOOT + prompt + "\r\n":
                    observer.feed(character)
                self.assertTrue(observer.feed(""))

    def test_only_exact_native_prefix_is_accepted_and_cannot_qualify_boot(self):
        variants = ("[WLAN-A] " + RADIO_PROMPT, "[WLAN-E]" + RADIO_PROMPT,
                    "[WLAN-E]  " + RADIO_PROMPT, "[WLAN-E] [WLAN-E] " + RADIO_PROMPT,
                    "prefix [WLAN-E] " + RADIO_PROMPT, "[WLAN-E] " + RADIO_PROMPT + "!")
        for prompt in variants:
            with self.subTest(prompt=prompt):
                observer = capture.RadioRamPrompt()
                self.assertFalse(observer.feed(RADIO_BOOT + prompt))
        observer = capture.RadioRamPrompt()
        self.assertFalse(observer.feed("[WLAN-E] " + RADIO_BOOT + "[WLAN-E] " + RADIO_PROMPT))

    def test_exact_boot_then_byte_split_prompt_without_newline_is_recognized(self):
        observer = capture.RadioRamPrompt()
        for character in "\r" + RADIO_BOOT + "\r" + RADIO_PROMPT:
            observer.feed(character)
        self.assertTrue(observer.feed(""))
        self.assertTrue(observer.qualified_boot_observed)

    def test_prompt_may_wrap_at_the_two_sentence_boundary(self):
        observer = capture.RadioRamPrompt()
        prompt = RADIO_PROMPT.replace(". Press", ".\r\n\rPress")
        self.assertTrue(observer.feed(RADIO_BOOT + prompt + "\r\n"))

    def test_only_complete_exact_diagnostic_boot_arms_confirmation(self):
        variants = (BOOT, RADIO_BOOT.rstrip("\n"),
                    RADIO_BOOT.replace("firmware=1", "firmware=2"),
                    RADIO_BOOT.replace("protocol=1", "protocol=2"),
                    RADIO_BOOT.replace("ble_display", "bringup"),
                    RADIO_BOOT.replace("1500000", "115200"),
                    RADIO_BOOT.replace("v4-radio-preinit", "v3-radio-preinit"),
                    RADIO_BOOT.replace("firmware=1", "firmware=1 firmware=1"),
                    RADIO_BOOT.rstrip("\n") + " extra\n", "prefix " + RADIO_BOOT)
        for boot in variants:
            with self.subTest(boot=boot):
                observer = capture.RadioRamPrompt()
                observer.feed(boot)
                self.assertFalse(observer.feed(RADIO_PROMPT))

    def test_prompt_before_boot_or_after_unknown_boot_cannot_confirm(self):
        for text in (RADIO_PROMPT + "\n" + RADIO_BOOT,
                     RADIO_BOOT + BOOT + RADIO_PROMPT,
                     RADIO_BOOT + "NU87|BOOT app=bringup\n" + RADIO_PROMPT,
                     ACK + SEND + RADIO_PROMPT):
            with self.subTest(text=text):
                observer = capture.RadioRamPrompt()
                self.assertFalse(observer.feed(text))

    def test_similar_incomplete_or_suffixed_prompts_are_rejected(self):
        variants = (RADIO_PROMPT[:-1], RADIO_PROMPT.lower(),
                    RADIO_PROMPT.replace("Wifi", "WiFi"),
                    RADIO_PROMPT.replace(" ignore ", " ignore  "),
                    "prefix " + RADIO_PROMPT, RADIO_PROMPT + "!", RADIO_PROMPT + "X")
        for prompt in variants:
            with self.subTest(prompt=prompt):
                observer = capture.RadioRamPrompt()
                self.assertFalse(observer.feed(RADIO_BOOT + prompt))

    def test_later_chunk_suffix_revokes_unterminated_prompt(self):
        observer = capture.RadioRamPrompt()
        self.assertTrue(observer.feed(RADIO_BOOT + RADIO_PROMPT))
        self.assertFalse(observer.feed("X"))
        self.assertFalse(observer.feed(""))

    def test_long_similar_lines_cannot_turn_into_exact_prompt_when_history_is_bounded(self):
        observer = capture.RadioRamPrompt()
        observer.feed(RADIO_BOOT)
        self.assertFalse(observer.feed("X" * 1500 + RADIO_PROMPT + "\n"))
        self.assertFalse(observer.feed("Y" * (1024 - len(RADIO_PROMPT) - 2) + "\n"))
        self.assertFalse(observer.feed(""))

    def test_partial_second_boot_blocks_permission_from_previous_session(self):
        for prefix in ("N", "NU87|BO", "NU87|BOOT app=bringup"):
            with self.subTest(prefix=prefix):
                observer = capture.RadioRamPrompt()
                observer.feed(RADIO_BOOT + RADIO_PROMPT + "\n")
                self.assertFalse(observer.feed(prefix))
                self.assertFalse(observer.feed(""))


class CaptureTests(unittest.TestCase):
    def run_capture(self, payload, error=None, ports=None, arguments=None, close_error=None,
                    write_result=1, write_error=None):
        operations = []
        chunks = [payload] if isinstance(payload, str) else payload

        class Port:
            def __init__(self):
                object.__setattr__(self, "chunks", iter(chunks))
                object.__setattr__(self, "in_waiting", 4096)

            def __setattr__(self, name, value):
                if name in ("dtr", "rts", "port"):
                    operations.append((name, value))
                object.__setattr__(self, name, value)

            def open(self):
                operations.append(("open",))

            def read(self, _size):
                try:
                    data = next(self.chunks).encode()
                except StopIteration:
                    if error is not None:
                        raise error from None
                    data = b""
                operations.append(("read", data))
                return data

            def write(self, data):
                operations.append(("write", data))
                if write_error is not None:
                    raise write_error
                return write_result

            def close(self):
                operations.append(("close",))
                if close_error is not None:
                    raise close_error

            # No reset_input_buffer, send_break or reset methods exist.

        def construct(**kwargs):
            operations.append(("construct", kwargs))
            return Port()

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "nested/sender.log"
            args = ["--serial-number", SERIAL, "--output", str(output)]
            if arguments:
                args.extend(arguments)
            with patch("serial.tools.list_ports.comports",
                       return_value=[target()] if ports is None else ports), \
                    patch("serial.Serial", side_effect=construct) as serial_factory, \
                    patch.object(capture.time, "monotonic", side_effect=itertools.count(0, 0.01)), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = capture.main(args)
            text = output.read_text()
            result = json.loads(output.with_suffix(".json").read_text())
        return code, text, result, serial_factory, operations

    def test_default_thirty_second_capture_opens_only_after_modem_lines_are_false(self):
        code, text, result, factory, operations = self.run_capture(BOOT + ACK + SEND)
        expected_arguments = {"port": None, "baudrate": 1500000, "timeout": 0.1,
                              "rtscts": False, "dsrdtr": False, "xonxoff": False}
        factory.assert_called_once_with(**expected_arguments)
        self.assertEqual(operations[:5], [("construct", expected_arguments),
                                         ("dtr", False), ("rts", False),
                                         ("port", DEVICE), ("open",)])
        self.assertEqual(operations[-1], ("close",))
        self.assertEqual(code, 0)
        self.assertEqual(result["requested_seconds"], 30)
        self.assertTrue(result["capture_completed"])
        self.assertEqual(result["baud"], 1500000)
        self.assertFalse(result["dtr"])
        self.assertFalse(result["rts"])
        self.assertFalse(result["serial_writes"])
        self.assertEqual(result["serial_write_records"], [])
        self.assertFalse(result["radio_ram_confirmation"]["enabled"])
        self.assertFalse(any(operation[0] == "write" for operation in operations))
        self.assertFalse(result["reset_requested"])
        self.assertEqual(result["usb_identity"], {"vid": "10c4", "pid": "ea60",
                                                  "serial_number": SERIAL, "matched": True})
        self.assertIn(ACK.strip(), text)
        self.assertIn("unverified", result["overall_physical_result"])

    def test_completed_unrecognized_capture_returns_one_and_keeps_json(self):
        code, _text, result, _factory, _operations = self.run_capture("NU87 bringup heartbeat=2\n")
        self.assertEqual(code, 1)
        self.assertTrue(result["capture_completed"])
        self.assertFalse(result["firmware_recognized"])
        self.assertIsNone(result["capture_error"])
        self.assertEqual(result["exit_code"], 1)

    def test_ctrl_c_retains_partial_line_and_json(self):
        code, text, result, _factory, operations = self.run_capture(
            BOOT + ACK.rstrip("\n"), error=KeyboardInterrupt())
        self.assertEqual(code, 130)
        self.assertIn(ACK.strip(), text)
        self.assertTrue(result["interrupted"])
        self.assertFalse(result["capture_completed"])
        self.assertTrue(result["firmware_recognized"])
        self.assertEqual(result["ack_ok_records"], 1)
        self.assertIn("partial evidence", result["capture_error"])
        self.assertEqual(operations[-1], ("close",))

    def test_usb_read_error_keeps_partial_evidence_and_closes_port(self):
        code, text, result, _factory, operations = self.run_capture(
            BOOT + SEND.rstrip("\n"), error=OSError("USB removed"))
        self.assertEqual(code, 2)
        self.assertIn(SEND.strip(), text)
        self.assertFalse(result["interrupted"])
        self.assertFalse(result["capture_completed"])
        self.assertEqual(result["send_success_records"], 1)
        self.assertIn("USB removed", result["capture_error"])
        self.assertEqual(operations[-1], ("close",))

    def test_identity_failure_never_constructs_serial_even_with_explicit_port(self):
        cases = ([target(vid=0x2FE3)], [target(pid=4)], [target(serial_number="other")],
                 [target(), target(device="/dev/ttyUSB8")], [])
        for ports in cases:
            with self.subTest(ports=ports):
                code, _text, result, factory, operations = self.run_capture(
                    "", ports=ports, arguments=["--port", DEVICE])
                self.assertEqual(code, 2)
                self.assertFalse(result["usb_identity"]["matched"])
                self.assertFalse(result["capture_completed"])
                self.assertFalse(result["firmware_recognized"])
                factory.assert_not_called()
                self.assertEqual(operations, [])

    def test_explicit_wrong_port_cannot_open_matching_bridge_at_other_path(self):
        code, _text, result, factory, operations = self.run_capture(
            "", arguments=["--port", "/dev/ttyUSB0", "--seconds", "20"])
        self.assertEqual(code, 2)
        self.assertFalse(result["usb_identity"]["matched"])
        factory.assert_not_called()
        self.assertEqual(operations, [])

    def test_close_error_prevents_completed_success(self):
        code, _text, result, _factory, operations = self.run_capture(
            BOOT, close_error=OSError("bridge disappeared during close"))
        self.assertEqual(code, 2)
        self.assertFalse(result["capture_completed"])
        self.assertTrue(result["firmware_recognized"])
        self.assertIn("Serial close error", result["capture_error"])
        self.assertEqual(operations[-1], ("close",))

    def test_opt_in_confirms_exact_byte_split_prompt_once_after_a_quiet_read(self):
        payload = list("\r" + RADIO_BOOT + "\r" + RADIO_PROMPT)
        payload += ["", "\n" + RADIO_PROMPT + "\n" + RADIO_BOOT + RADIO_PROMPT]
        code, text, result, factory, operations = self.run_capture(
            payload, arguments=["--confirm-radio-ram"])
        self.assertEqual(code, 0)
        self.assertEqual(factory.call_args.kwargs["write_timeout"], 1)
        self.assertEqual(operations[1:5], [("dtr", False), ("rts", False),
                                          ("port", DEVICE), ("open",)])
        writes = [index for index, operation in enumerate(operations) if operation[0] == "write"]
        self.assertEqual(len(writes), 1)
        self.assertEqual(operations[writes[0]], ("write", b"\n"))
        self.assertEqual(operations[writes[0] - 1], ("read", b""))
        self.assertTrue(result["serial_writes"])
        confirmation = result["radio_ram_confirmation"]
        self.assertTrue(confirmation["enabled"])
        self.assertTrue(confirmation["qualified_boot_observed"])
        self.assertTrue(confirmation["write_attempted"])
        self.assertTrue(confirmation["confirmation_byte_written"])
        self.assertEqual(result["serial_write_records"][0]["written_bytes"], 1)
        self.assertIsNone(result["serial_write_records"][0]["error"])
        self.assertEqual(result["reported_boot"]["diagnostic"], "v4-radio-preinit")
        self.assertIn(RADIO_PROMPT, text)
        self.assertFalse(result["reset_requested"])

    def test_actual_v4_wifi_log_confirms_once_then_retains_ble_result(self):
        payload = [RADIO_BOOT + "NU87|RADIO device_ready=1 timeout_ms=15000 "
                   "mode=station_no_join\n[WLAN-A] Init WIFI\n[WLAN-E] Efuse emp",
                   "ty! Wifi performance may be affected. Press any key to ignore and continue\n",
                   "", "NU87|RADIO ready=1 elapsed_ms=100\nNU87|BLE enable_result=0\n"]
        code, _text, result, _factory, operations = self.run_capture(
            payload, arguments=["--confirm-radio-ram"])
        self.assertEqual(code, 0)
        self.assertTrue(result["radio_ram_confirmation"]["confirmation_byte_written"])
        self.assertTrue(result["ble_enable_succeeded"])
        self.assertEqual([item for item in operations if item[0] == "write"], [("write", b"\n")])

    def test_no_option_wrong_firmware_or_inexact_prompt_never_writes(self):
        cases = ((RADIO_BOOT + RADIO_PROMPT, []),
                 (RADIO_BOOT + "[WLAN-E] " + RADIO_PROMPT, []),
                 (BOOT + "[WLAN-E] " + RADIO_PROMPT, ["--confirm-radio-ram"]),
                 ("[WLAN-E] " + RADIO_PROMPT, ["--confirm-radio-ram"]),
                 (BOOT + RADIO_PROMPT, ["--confirm-radio-ram"]),
                 (RADIO_PROMPT, ["--confirm-radio-ram"]),
                 (RADIO_PROMPT + "\n" + RADIO_BOOT, ["--confirm-radio-ram"]),
                 (RADIO_BOOT + RADIO_PROMPT[:-1], ["--confirm-radio-ram"]),
                 (RADIO_BOOT + "prefix " + RADIO_PROMPT, ["--confirm-radio-ram"]),
                 ([RADIO_BOOT + RADIO_PROMPT, "X"], ["--confirm-radio-ram"]),
                 (RADIO_BOOT + BOOT + RADIO_PROMPT, ["--confirm-radio-ram"]))
        for payload, arguments in cases:
            with self.subTest(payload=payload, arguments=arguments):
                _code, _text, result, _factory, operations = self.run_capture(
                    payload, arguments=arguments)
                self.assertFalse(result["serial_writes"])
                self.assertEqual(result["serial_write_records"], [])
                self.assertFalse(result["radio_ram_confirmation"]["write_attempted"])
                self.assertFalse(any(operation[0] == "write" for operation in operations))

    def test_confirmation_failure_or_ctrl_c_preserves_exact_attempt_without_retry(self):
        cases = ((0, None, 2, 0), (1, OSError("write failed"), 2, None),
                 (1, KeyboardInterrupt(), 130, None))
        for write_result, write_error, expected_code, written in cases:
            with self.subTest(write_result=write_result, write_error=write_error):
                code, text, result, _factory, operations = self.run_capture(
                    RADIO_BOOT + RADIO_PROMPT, arguments=["--confirm-radio-ram"],
                    write_result=write_result, write_error=write_error)
                self.assertEqual(code, expected_code)
                self.assertIn(RADIO_PROMPT, text)
                self.assertTrue(result["serial_writes"])
                self.assertEqual(len(result["serial_write_records"]), 1)
                record = result["serial_write_records"][0]
                self.assertEqual(record["written_bytes"], written)
                self.assertFalse(record["completed"])
                self.assertIsNotNone(record["error"])
                self.assertFalse(result["radio_ram_confirmation"]["confirmation_byte_written"])
                self.assertEqual([operation for operation in operations if operation[0] == "write"],
                                 [("write", b"\n")])
                self.assertEqual(operations[-1], ("close",))

    def test_prompt_before_main_boot_is_recorded_without_sending_confirmation(self):
        code, _text, result, _factory, operations = self.run_capture(
            [RADIO_PROMPT, "", "\n" + RADIO_BOOT], arguments=["--confirm-radio-ram"])
        self.assertEqual(code, 0)
        confirmation = result["radio_ram_confirmation"]
        self.assertTrue(confirmation["exact_prompt_observed_without_qualified_boot"])
        self.assertTrue(confirmation["exact_prompt_observed_before_qualified_boot"])
        self.assertTrue(confirmation["qualified_boot_observed"])
        self.assertFalse(confirmation["write_attempted"])
        self.assertFalse(result["serial_writes"])
        self.assertFalse(any(operation[0] == "write" for operation in operations))


if __name__ == "__main__":
    unittest.main()
