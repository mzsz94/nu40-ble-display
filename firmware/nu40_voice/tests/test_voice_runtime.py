"""Run actual NU40 BLE callbacks with a deterministic deferred workqueue."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class VoiceRuntimeTests(unittest.TestCase):
    def test_queued_packets_cancel_on_stop_restart_and_transport_failure(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("A host C compiler is required")
        source = Path(__file__).resolve()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for header in ("audio/dmic.h", "bluetooth/conn.h", "bluetooth/gatt.h",
                           "device.h", "drivers/gpio.h", "kernel.h", "sys/printk.h"):
                target = root / "zephyr" / header
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('#include "zephyr_runtime_stub.h"\n')
            executable = root / "voice-runtime-test"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-fsanitize=address,undefined",
                            "-I", str(root), "-I", str(source.parent),
                            "-I", str(source.parents[2] / "voice_common"),
                            str(source.with_suffix(".c")), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
