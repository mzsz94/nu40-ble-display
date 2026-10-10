# SPDX-License-Identifier: Apache-2.0
"""Verify BLE display evidence and read-only partial capture without accessing devices."""

import argparse
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import ble_display_capture as capture

BOOT = "LINK|BOOT role=display firmware=1 oled_init=0 protocol=1\n"
RX = "LINK|RX seq=7 bytes=5 oled_result=0 ack_result=0 visible=unverified\n"


def status(rx=0, writes=2, ack=0, errors=0, connected=0):
    return (f"LINK|STATUS role=display connected={connected} oled_ready=1 rx={rx} "
            f"oled_writes={writes} ack_queued={ack} errors={errors}\n")


def target(**changes):
    fields = {"device": "/dev/ttyACM1", "vid": 0x2FE3, "pid": 4,
              "product": capture.PRODUCT, "serial_number": capture.TARGET_SERIAL}
    fields.update(changes)
    return SimpleNamespace(**fields)


class ParserTests(unittest.TestCase):
    def test_ota_receiver_v3_preserves_driver_evidence_and_exact_usb_selection(self):
        result = capture.summarize(BOOT.replace("firmware=1", "firmware=3") + RX)
        self.assertEqual(result["firmware_version"], 3)
        self.assertEqual(result["successful_oled_driver_records"], 1)
        self.assertEqual(capture.select_port([target(product="NU40DK BLE display OTA v3")]),
                         "/dev/ttyACM1")
        self.assertIn("unverified", result["oled_visible"])

    def test_face_receiver_v2_keeps_driver_and_physical_evidence_separate(self):
        result = capture.summarize(BOOT.replace("firmware=1", "firmware=2") +
                                   RX.replace("bytes=5", "bytes=10"))
        self.assertTrue(result["firmware_recognized"])
        self.assertEqual(result["firmware_version"], 2)
        self.assertEqual(result["successful_oled_driver_records"], 1)
        self.assertIn("unverified", result["oled_visible"])

    def test_v2_usb_product_requires_same_exact_vid_pid_and_serial(self):
        self.assertEqual(capture.select_port([target(product="NU40DK BLE display v2")]),
                         "/dev/ttyACM1")
        with self.assertRaises(ValueError):
            capture.select_port([target(product="NU40DK BLE display v2", serial_number="other")])

    def test_status_and_rx_after_missed_boot_recognize_display(self):
        for text in (status(), RX):
            with self.subTest(text=text):
                result = capture.summarize(text)
                self.assertTrue(result["firmware_recognized"])
                self.assertFalse(result["firmware_boot_observed"])
                self.assertIsNone(result["firmware_version"])
                self.assertIn("unverified", result["oled_visible"])

    def test_boot_identity_does_not_imply_any_rx_or_visible_success(self):
        result = capture.summarize(BOOT)
        self.assertTrue(result["firmware_recognized"])
        self.assertEqual(result["firmware_version"], 1)
        self.assertEqual(result["complete_rx_records"], 0)
        self.assertEqual(result["successful_oled_driver_records"], 0)
        self.assertIsNone(result["rx_counter_advanced"])
        self.assertIn("unverified", result["overall_physical_result"])

    def test_counters_must_advance_during_capture(self):
        result = capture.summarize(status(rx=14, writes=19, ack=14) * 2)
        self.assertFalse(result["rx_counter_advanced"])
        self.assertEqual(result["successful_oled_driver_records"], 0)
        result = capture.summarize(status() + RX + status(1, 3, 1, connected=1))
        self.assertTrue(result["rx_counter_advanced"])
        self.assertTrue(result["connected_observed"])
        self.assertEqual(result["successful_oled_driver_records"], 1)
        self.assertEqual(result["successful_oled_and_ack_queue_records"], 1)
        self.assertIn("unverified", result["ack_delivery"])

    def test_counter_reset_does_not_invent_continuous_progress(self):
        result = capture.summarize(status(9) + status(0) + status(10))
        self.assertIsNone(result["rx_counter_advanced"])
        self.assertTrue(result["counters"]["rx"]["decrease_observed"])

    def test_driver_and_notification_errors_remain_separate(self):
        text = BOOT + RX.replace("ack_result=0", "ack_result=-12")
        text += RX.replace("oled_result=0", "oled_result=-5")
        result = capture.summarize(text)
        self.assertTrue(result["rx_driver_error_observed"])
        self.assertEqual(result["successful_oled_driver_records"], 1)
        self.assertEqual(result["successful_ack_queue_records"], 1)
        self.assertEqual(result["successful_oled_and_ack_queue_records"], 0)

    def test_explicit_unknown_boot_cannot_fallback_to_valid_status(self):
        for boot in (BOOT.replace("firmware=1", "firmware=10"),
                     BOOT.replace("role=display", "role=source"),
                     BOOT.replace("protocol=1", "protocol=2")):
            with self.subTest(boot=boot):
                result = capture.summarize(boot + status() + RX)
                self.assertFalse(result["firmware_recognized"])
                self.assertEqual(result["successful_oled_driver_records"], 0)

    def test_incomplete_malformed_and_other_firmware_do_not_count(self):
        variants = ("NU40DK BLE display v1\n", "KIT|STATUS oled_write_ok=2\n",
                    "LINK|RX seq=7 bytes=5 oled_result=0\n",
                    RX.replace("bytes=5", "bytes=5garbage"),
                    RX.replace("seq=7", "seq=65536"), RX.replace("bytes=5", "bytes=0"),
                    RX.replace("seq=7", "seq=7 seq=8"),
                    status().replace("errors=0", "errors=nan"),
                    status().replace("connected=0", "connected=2"),
                    status().replace("role=display", "role=source"))
        for text in variants:
            with self.subTest(text=text):
                result = capture.summarize(text)
                self.assertFalse(result["firmware_recognized"])
                self.assertEqual(result["successful_oled_driver_records"], 0)

    def test_interleaved_tail_does_not_supply_missing_fields(self):
        text = "LINK|RX seq=7 bytes=5 oled_result=0 ack_res"
        text += status(rx=1)
        result = capture.summarize(text)
        self.assertFalse(result["firmware_recognized"])
        self.assertEqual(result["complete_rx_records"], 0)
        self.assertEqual(result["malformed_or_incomplete_records"], 1)

    def test_rejections_and_ansi_are_preserved_as_distinct_evidence(self):
        result = capture.summarize("\x1b[32m" + BOOT + "\x1b[0m"
                                   + "LINK|RX rejected=-2 queue_result=0\n")
        self.assertEqual(result["rejected_rx_records"], [{"rejected": -2, "queue_result": 0}])
        self.assertEqual(result["successful_oled_driver_records"], 0)


class IdentityTests(unittest.TestCase):
    def test_all_usb_identity_fields_and_explicit_port_are_mandatory(self):
        self.assertEqual(capture.select_port([target()]), "/dev/ttyACM1")
        variants = ({"product": capture.PRODUCT + "0"}, {"product": "NU40DK KIT selftest v3"},
                    {"vid": 0x10C4}, {"pid": 5}, {"serial_number": "other"})
        for fields in variants:
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                capture.select_port([target(**fields)], port="/dev/ttyACM1")
        for ports, serial, port in (([], capture.TARGET_SERIAL, None),
                                    ([target(), target()], capture.TARGET_SERIAL, None),
                                    ([target()], "", None),
                                    ([target()], capture.TARGET_SERIAL, "/dev/ttyACM0")):
            with self.subTest(ports=ports), self.assertRaises(ValueError):
                capture.select_port(ports, serial_number=serial, port=port)

    def test_duration_bounds(self):
        self.assertEqual(capture.duration("20"), 20)
        self.assertEqual(capture.duration("300"), 300)
        for value in ("19", "301", "oops", "20.5"):
            with self.assertRaises(argparse.ArgumentTypeError):
                capture.duration(value)


class PersistenceTests(unittest.TestCase):
    def run_capture(self, payload, error=None, ports=None):
        class Port:
            in_waiting = len(payload)

            def __enter__(self):
                self.first = True
                return self

            def __exit__(self, *_args):
                return False

            def read(self, _size):
                if self.first:
                    self.first = False
                    return payload.encode()
                if error is not None:
                    raise error
                return b""

            # Intentionally no write method: any command would fail the test.

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "display.log"
            with patch("serial.tools.list_ports.comports",
                       return_value=[target()] if ports is None else ports), \
                    patch("serial.Serial", return_value=Port()) as open_port, \
                    patch.object(capture.time, "monotonic", side_effect=range(500)), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = capture.main(["--seconds", "20", "--output", str(output)])
            text = output.read_text()
            result = json.loads(output.with_suffix(".json").read_text())
        return code, text, result, open_port

    def test_completed_capture_is_not_physical_pass(self):
        code, text, result, opened = self.run_capture(BOOT + status() + RX + status(1, 3, 1))
        self.assertEqual(code, 0)
        self.assertTrue(result["capture_completed"])
        self.assertFalse(result["serial_writes"])
        self.assertTrue(result["rx_counter_advanced"])
        self.assertIn("unverified", result["oled_visible"])
        self.assertIn("LINK|RX", text)
        opened.assert_called_once_with("/dev/ttyACM1", 115200, timeout=0.1)

    def test_no_firmware_records_cannot_return_zero(self):
        code, _text, result, _opened = self.run_capture("NU40DK BLE display v1\n")
        self.assertEqual(code, 1)
        self.assertTrue(result["capture_completed"])
        self.assertFalse(result["firmware_recognized"])

    def test_metadata_records_observed_v2_product(self):
        code, _text, result, _opened = self.run_capture(
            BOOT.replace("firmware=1", "firmware=2"),
            ports=[target(product="NU40DK BLE display v2")])
        self.assertEqual(code, 0)
        self.assertEqual(result["usb_identity"]["product"], "NU40DK BLE display v2")

    def test_ctrl_c_keeps_partial_line_and_json(self):
        code, text, result, _opened = self.run_capture(BOOT + RX.rstrip("\n"), KeyboardInterrupt())
        self.assertEqual(code, 130)
        self.assertIn(RX.strip(), text)
        self.assertTrue(result["firmware_recognized"])
        self.assertTrue(result["interrupted"])
        self.assertFalse(result["capture_completed"])
        self.assertEqual(result["successful_oled_driver_records"], 1)

    def test_usb_read_error_keeps_partial_evidence(self):
        code, text, result, _opened = self.run_capture(BOOT, OSError("USB removed"))
        self.assertEqual(code, 2)
        self.assertIn(BOOT.strip(), text)
        self.assertFalse(result["capture_completed"])
        self.assertIn("USB removed", result["capture_error"])

    def test_wrong_usb_identity_never_opens_any_port_and_still_writes_json(self):
        code, _text, result, opened = self.run_capture("", ports=[target(vid=0x10C4)])
        self.assertEqual(code, 2)
        self.assertFalse(result["usb_identity"]["matched"])
        self.assertIsNone(result["usb_identity"]["product"])
        self.assertFalse(result["firmware_recognized"])
        opened.assert_not_called()


if __name__ == "__main__":
    unittest.main()
