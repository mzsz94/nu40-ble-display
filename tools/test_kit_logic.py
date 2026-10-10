"""Exercise actual KIT button and WAV logic, including corruption and clock wrap."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class KitLogicTests(unittest.TestCase):
    def test_real_c_logic_with_sanitizers(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = Path(temporary) / "kit-test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            str(ROOT / "shared/tests/test_kit_logic.c"), "-o", str(exe)],
                           check=True, capture_output=True, text=True, timeout=60)
            result = subprocess.run([str(exe)], env=dict(os.environ,
                                    ASAN_OPTIONS="detect_leaks=0"),
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
