"""Compile and execute the wire codec with ASan/UBSan on the host."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class VoiceFrameTests(unittest.TestCase):
    def test_codec_vectors_bounds_and_sanitizers(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "Host C compiler is required")
        source = Path(__file__).with_suffix(".c")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "voice-frame-test"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(source.parent.parent), str(source), "-lm", "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
