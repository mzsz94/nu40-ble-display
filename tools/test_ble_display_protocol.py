"""Compile and run the shared wire protocol against the host C compiler."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BleDisplayProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            raise unittest.SkipTest("A host C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="ble-display-protocol-")
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / "test_ble_display_protocol"
        command = [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-O2",
            str(ROOT / "shared/tests/test_ble_display_protocol.c"),
            "-o",
            str(cls.executable),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"C protocol tests did not compile:\n{result.stdout}{result.stderr}")

    def run_case(self, name):
        result = subprocess.run(
            [str(self.executable), name],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), f"PASS {name}")

    def test_fixed_wire_vectors(self):
        self.run_case("wire_vectors")

    def test_ascii_and_line_feed_validation(self):
        self.run_case("text_validation")

    def test_oled_wrap_at_every_supported_length(self):
        self.run_case("layout_wrap_boundaries")

    def test_oled_explicit_newlines_and_fifth_row(self):
        self.run_case("layout_explicit_newlines")

    def test_oled_invalid_text_inputs(self):
        self.run_case("layout_invalid_input")

    def test_every_supported_text_length(self):
        self.run_case("all_text_lengths")

    def test_invalid_builder_inputs(self):
        self.run_case("builder_rejects_invalid_input")

    def test_malformed_frame_cancels_partial_transfer(self):
        self.run_case("malformed_frames_cancel_assembly")

    def test_fragment_order_sequence_and_total(self):
        self.run_case("fragment_order_and_identity")

    def test_reset_and_first_fragment_restart(self):
        self.run_case("reset_and_restart")

    def test_duplicate_fragments_after_completion(self):
        self.run_case("duplicates_after_completion")

    def test_timeout_boundary_and_clock_wrap(self):
        self.run_case("timeout_and_clock_wrap")

    def test_ack_validation_and_output_preservation(self):
        self.run_case("ack_validation")


if __name__ == "__main__":
    unittest.main()
