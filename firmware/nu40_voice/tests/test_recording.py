"""Exercise local clip ownership independently of the BLE transport."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class RecordingTests(unittest.TestCase):
    def test_capture_never_sends_before_submit_and_cancel_discards_every_frame(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("A host C compiler is required")
        source = Path(__file__).resolve()
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "recording-test"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(source.parents[1] / "src"),
                            "-I", str(source.parents[2] / "voice_common"),
                            str(source.with_suffix(".c")), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
