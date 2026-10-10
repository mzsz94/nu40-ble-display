"""Exercise face wire validation and the actual OLED rasterizer under sanitizers."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BleFaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("A host C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="ble-face-")
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / "face-test"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             str(ROOT / "shared/tests/test_ble_face.c"), "-o", str(cls.executable)],
            capture_output=True, text=True, check=False, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, name):
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        result = subprocess.run([str(self.executable), name], env=environment,
                                capture_output=True, text=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), f"PASS {name}")

    def test_fixed_wire_vectors_and_text_compatibility(self):
        self.run_case("wire_vectors")

    def test_all_field_values_lengths_and_nulls(self):
        self.run_case("malformed")

    def test_rejected_input_preserves_outputs(self):
        self.run_case("output_preservation")

    def test_all_expressions_at_extreme_positions_and_openness(self):
        self.run_case("render_boundaries")

    def test_animation_blinks_changes_expression_and_wraps_clock(self):
        self.run_case("demo_states")


if __name__ == "__main__":
    unittest.main()
