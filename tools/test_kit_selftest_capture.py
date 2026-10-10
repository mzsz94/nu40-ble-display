# SPDX-License-Identifier: Apache-2.0
"""Check serial identity, physical-evidence limits and interrupted capture persistence."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from kit_selftest_capture import DEFAULT_PRODUCT, main, select_port, summarize

BOOT = "KIT|BOOT NU40DK-KIT selftest firmware=1 observation=pending\n"
STATUS = ("KIT|STATUS keys_ready=0xf pressed=0x0 seen=0xf oled_write_ok=10 "
          "mic_blocks=100 mic_errors=0 beep_tx_ok=1 beep_error=0\n")


def mic(peak, rms, blocks):
    return (f"KIT|MIC rate=16000 width=16 channels=1 peak={peak} rms={rms} "
            f"blocks={blocks} sound_response=unverified\n")


class EvidenceTests(unittest.TestCase):
    def test_imu_and_legacy_logs_never_recognize_new_selftest(self):
        for text in ("", "NU40-DK IMU v1.1\nstatus: command not found\n",
                     "STATUS OLED=1 SPEAKER=1 KIT_KEYS=0,0,0,0\n"):
            with self.subTest(text=text):
                result = summarize(text)
                self.assertFalse(result["selftest_firmware_recognized"])
                self.assertFalse(result["all_four_key_presses_observed"])
                self.assertIsNone(result["speaker_transfer_succeeded"])

    def test_cumulative_seen_before_capture_does_not_count_as_press(self):
        result = summarize(BOOT + STATUS)
        self.assertEqual(result["keys_pressed_during_capture"], [])
        self.assertFalse(result["all_four_key_presses_observed"])
        self.assertIsNone(result["speaker_transfer_succeeded"])

    def test_actual_key_masks_and_init_results_remain_separate(self):
        text = BOOT + "KIT|OLED init_result=-5 addr=0x3c visible=unverified\n"
        text += "KIT|SPEAKER tx_result=0 nominal_hz=500 audible=unverified\n"
        text += "".join(f"KIT|BUTTON pressed=0x{1 << i:x} seen=0xf\n" for i in range(4))
        result = summarize(text)
        self.assertTrue(result["all_four_key_presses_observed"])
        self.assertEqual(result["oled_init_result"], -5)
        self.assertTrue(result["speaker_transfer_succeeded"])
        self.assertEqual(result["speaker_audible"], "requires listening")
        self.assertIn("unverified", result["overall_physical_result"])

    def test_periodic_signature_works_after_boot_was_missed(self):
        result = summarize(STATUS + mic(100, 20, 100))
        self.assertTrue(result["selftest_firmware_recognized"])
        self.assertFalse(result["firmware_boot_observed"])

    def test_level_rise_is_evidence_and_never_acoustic_pass(self):
        text = BOOT + "[HOST] PHASE silence\n" + mic(100, 10, 100) + mic(120, 12, 200)
        text += "[HOST] PHASE voice\n"
        text += "".join(mic(2000, 500, 300 + i * 100) for i in range(3))
        result = summarize(text)
        self.assertEqual(result["microphone"]["response_evidence"], "level_increase_observed")
        self.assertTrue(result["microphone"]["blocks_advanced"])
        self.assertIn("unverified", result["microphone_acoustic_capture"])

    def test_constant_values_and_advancing_blocks_do_not_prove_sound_response(self):
        text = BOOT + "[HOST] PHASE silence\n" + mic(1000, 500, 1) + mic(1000, 500, 2)
        text += "[HOST] PHASE voice\n"
        text += "".join(mic(1000, 500, 3 + i) for i in range(3))
        self.assertEqual(summarize(text)["microphone"]["response_evidence"],
                         "no_clear_level_increase")

    def test_unlabelled_samples_and_read_errors_do_not_show_response(self):
        result = summarize(BOOT + mic(100, 10, 1) + mic(1000, 500, 2))
        self.assertEqual(result["microphone"]["response_evidence"], "insufficient_phase_samples")
        result = summarize(BOOT + "KIT|MIC read_error=-5 other_tests_continue=1\n")
        self.assertTrue(result["microphone_read_error_observed"])
        self.assertEqual(result["microphone"]["response_evidence"], "stream_error_observed")

    def test_transfer_failures_do_not_become_successful_physical_checks(self):
        result = summarize(BOOT + "KIT|SPEAKER tx_result=-5 audible=unverified\n")
        self.assertFalse(result["speaker_transfer_succeeded"])
        self.assertTrue(result["speaker_test_attempted"])

    def test_zero_or_unchanged_counters_are_not_transfer_failures(self):
        for count in (0, 3):
            with self.subTest(count=count):
                status = STATUS.replace("beep_tx_ok=1", f"beep_tx_ok={count}")
                result = summarize(BOOT + status * 3)
                self.assertIsNone(result["speaker_transfer_succeeded"])
                if count == 0:
                    self.assertFalse(result["speaker_test_attempted"])
                else:
                    self.assertIsNone(result["speaker_test_attempted"])

    def test_counter_increase_is_transfer_evidence(self):
        result = summarize(BOOT + STATUS.replace("beep_tx_ok=1", "beep_tx_ok=0") + STATUS)
        self.assertTrue(result["speaker_transfer_succeeded"])
        self.assertTrue(result["speaker_test_attempted"])
        self.assertEqual(result["speaker_audible"], "requires listening")

    def test_late_microphone_increase_is_recorded_without_changing_median_result(self):
        text = BOOT + "[HOST] PHASE silence\n" + mic(975, 229, 500) + mic(961, 216, 600)
        text += "[HOST] PHASE voice\n"
        text += mic(849, 195, 700) + mic(1239, 319, 801) + mic(1759, 364, 901)
        text += mic(7165, 1004, 1001) + mic(5838, 669, 1101)
        result = summarize(text)
        evidence = result["microphone"]
        self.assertEqual(evidence["response_evidence"], "no_clear_level_increase")
        self.assertEqual(evidence["phase_statistics"]["voice"]["max_rms"], 1004)
        self.assertEqual(evidence["phase_statistics"]["voice"]["max_peak"], 7165)
        self.assertEqual(evidence["voice_windows_with_level_increase"], 2)
        self.assertIn("unverified", result["microphone_acoustic_capture"])


class PortTests(unittest.TestCase):
    def test_product_and_serial_choose_reenumerated_port(self):
        ports = [SimpleNamespace(device="/dev/ttyACM2", product=DEFAULT_PRODUCT + " v3",
                                 serial_number="abc")]
        self.assertEqual(select_port(ports, serial_number="abc")[0], "/dev/ttyACM2")

    def test_default_identity_can_fallback_but_explicit_filters_cannot(self):
        self.assertEqual(select_port([])[0], "/dev/ttyACM0")
        with self.assertRaises(ValueError):
            select_port([], serial_number="abc")
        with self.assertRaises(ValueError):
            select_port([], product="Other device")

    def test_multiple_matches_require_disambiguation(self):
        port = SimpleNamespace(device="/dev/ttyACM1", product=DEFAULT_PRODUCT, serial_number=None)
        with self.assertRaises(ValueError):
            select_port([port, port])


class SDEvidenceTests(unittest.TestCase):
    def test_supported_firmware_versions_are_recognized_and_reported(self):
        for version in (1, 2, 3):
            with self.subTest(version=version):
                result = summarize(BOOT.replace("firmware=1", f"firmware={version}"))
                self.assertTrue(result["selftest_firmware_recognized"])
                self.assertEqual(result["firmware_version"], version)

    def test_unknown_version_does_not_match_version_one_by_prefix(self):
        result = summarize(BOOT.replace("firmware=1", "firmware=10") + STATUS + mic(100, 10, 1))
        self.assertFalse(result["selftest_firmware_recognized"])
        self.assertEqual(result["firmware_version"], 10)

    def test_sector_read_and_capacity_survive_periodic_summary(self):
        text = BOOT + "KIT|SD detect_result=1 polarity=active_high readonly=1\n"
        text += "KIT|SD init_result=0 readonly=1\n"
        text += ("KIT|SD read_result=0 sectors=15646720 sector_bytes=512 "
                 "lba=0 count=1 readonly=1\n")
        text += "KIT|SD present=1 read_result=0 readonly=1\n"
        sd = summarize(text)["microsd"]
        self.assertTrue(sd["card_present"])
        self.assertEqual(sd["initialization_result"], 0)
        self.assertTrue(sd["sector0_read_succeeded"])
        self.assertEqual(sd["capacity_bytes_reported"], 8011120640)
        self.assertEqual(sd["read_lba_reported"], 0)
        self.assertEqual(sd["read_sector_count_reported"], 1)
        self.assertTrue(sd["read_only_reported"])
        self.assertIn("not tested", sd["filesystem"])
        self.assertIn("not verified", sd["whole_card_integrity"])

    def test_detection_and_initialization_do_not_prove_sector_read(self):
        text = BOOT + "KIT|SD detect_result=1 polarity=active_high readonly=1\n"
        text += "KIT|SD init_result=0 readonly=1\n"
        for pending in (-119, -115):
            with self.subTest(pending=pending):
                sd = summarize(text + f"KIT|SD present=1 read_result={pending} readonly=1\n"
                               )["microsd"]
                self.assertTrue(sd["card_present"])
                self.assertTrue(sd["read_in_progress"])
                self.assertIsNone(sd["sector0_read_succeeded"])

    def test_absent_card_is_not_read_success(self):
        text = BOOT + "KIT|SD detect_result=0 polarity=active_high readonly=1\n"
        text += "KIT|SD skipped=1 other_tests_continue=1\n"
        # Zephyr SDK arm Picolibc sys/errno.h defines ENOMEDIUM=135, not Linux's 123.
        text += "KIT|SD present=0 read_result=-135 readonly=1\n"
        sd = summarize(text)["microsd"]
        self.assertFalse(sd["card_present"])
        self.assertFalse(sd["sector0_read_succeeded"])
        self.assertIsNone(sd["initialization_result"])

    def test_sd_errors_and_unexecuted_detection_stay_distinct(self):
        sd = summarize(BOOT + "KIT|SD present=-1 read_result=-119 readonly=1\n")["microsd"]
        self.assertIsNone(sd["card_present"])
        self.assertIsNone(sd["sector0_read_succeeded"])
        sd = summarize(BOOT + "KIT|SD init_result=-5 readonly=1\n"
                       "KIT|SD read_result=-5 sectors=0 sector_bytes=0 lba=0 count=1 readonly=1\n"
                       )["microsd"]
        self.assertEqual(sd["initialization_result"], -5)
        self.assertFalse(sd["sector0_read_succeeded"])
        self.assertIsNone(sd["capacity_bytes_reported"])

    def test_sd_logs_without_selftest_identity_do_not_supply_kit_evidence(self):
        sd = summarize("NU40-DK IMU v1.1\nKIT|SD present=1 read_result=0 readonly=1\n")["microsd"]
        self.assertFalse(sd["evidence_observed"])
        self.assertIsNone(sd["card_present"])
        self.assertIsNone(sd["sector0_read_succeeded"])

    def test_missing_sd_logs_and_late_attachment_are_supported(self):
        sd = summarize(BOOT)["microsd"]
        self.assertFalse(sd["evidence_observed"])
        self.assertIsNone(sd["read_result"])
        sd = summarize(STATUS + mic(100, 10, 1)
                       + "KIT|SD present=1 read_result=0 readonly=1\n")["microsd"]
        self.assertTrue(sd["sector0_read_succeeded"])
        self.assertIsNone(sd["capacity_bytes_reported"])

    def test_interleaved_sd_line_does_not_invent_missing_geometry(self):
        text = BOOT + "KIT|SD read_result=0 sectors=62333952 sector_byte"
        text += ("KIT|STATUS keys_ready=0xf pressed=0x0 seen=0x0 oled_write_ok=16 "
                 "mic_blocks=200 mic_errors=0 beep_tx_ok=0 beep_error=0\n")
        text += "KIT|SD present=1 read_result=0 readonly=1\n"
        result = summarize(text)
        sd = result["microsd"]
        self.assertTrue(sd["sector0_read_succeeded"])
        self.assertEqual(sd["sector_count_reported"], 62333952)
        self.assertIsNone(sd["sector_bytes_reported"])
        self.assertIsNone(sd["capacity_bytes_reported"])
        self.assertIsNone(sd["read_lba_reported"])
        self.assertIsNone(sd["read_sector_count_reported"])
        self.assertIn("unverified", result["overall_physical_result"])


class PersistenceTests(unittest.TestCase):
    def test_twenty_seconds_capture_keeps_three_read_only_phases(self):
        class Port:
            in_waiting = len(BOOT)

            def __enter__(self):
                self.first = True
                return self

            def __exit__(self, *_args):
                return False

            def read(self, _size):
                if self.first:
                    self.first = False
                    return BOOT.replace("firmware=1", "firmware=3").encode()
                return b""

            # No write method: attempts to send commands must fail this test.

        serial = SimpleNamespace(Serial=lambda *_args, **_kwargs: Port())
        modules = {"serial": serial, "serial.tools": SimpleNamespace(list_ports=None)}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with (patch.dict(sys.modules, modules), contextlib.redirect_stdout(io.StringIO()),
                  patch("kit_selftest_capture.time.monotonic", side_effect=range(100))):
                code = main(["--port", "/dev/test", "--seconds", "20", "--output", str(output)])
            text = output.read_text()
            result = json.loads(output.with_suffix(".json").read_text())
        self.assertEqual(code, 0)
        self.assertEqual(result["firmware_version"], 3)
        self.assertTrue(result["capture_completed"])
        self.assertIn("[HOST] PHASE keys", text)
        self.assertIn("[HOST] PHASE silence", text)
        self.assertIn("[HOST] PHASE voice", text)
        self.assertNotIn("[HOST] PHASE observe", text)

    def _interrupt_capture(self, error):
        class Port:
            in_waiting = len(BOOT)

            def __enter__(self):
                self.first = True
                return self

            def __exit__(self, *_args):
                return False

            def read(self, _size):
                if self.first:
                    self.first = False
                    return BOOT.encode()
                raise error

        serial = SimpleNamespace(Serial=lambda *_args, **_kwargs: Port())
        modules = {"serial": serial, "serial.tools": SimpleNamespace(list_ports=None)}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with (patch.dict(sys.modules, modules), contextlib.redirect_stdout(io.StringIO())):
                code = main(["--port", "/dev/test", "--seconds", "20", "--output", str(output)])
            text = output.read_text()
            summary = json.loads(output.with_suffix(".json").read_text())
        return code, text, summary

    def test_ctrl_c_preserves_log_and_json(self):
        code, text, result = self._interrupt_capture(KeyboardInterrupt())
        self.assertEqual(code, 130)
        self.assertIn(BOOT.strip(), text)
        self.assertTrue(result["selftest_firmware_recognized"])
        self.assertTrue(result["interrupted"])
        self.assertFalse(result["capture_completed"])

    def test_usb_read_failure_preserves_partial_evidence(self):
        code, text, result = self._interrupt_capture(OSError("USB removed"))
        self.assertEqual(code, 2)
        self.assertIn(BOOT.strip(), text)
        self.assertIn("USB removed", result["capture_error"])
        self.assertFalse(result["capture_completed"])


if __name__ == "__main__":
    unittest.main()
