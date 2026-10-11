"""Compile and exercise the actual OLED parser/rasterizer with host sanitizers."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class GraphicsTests(unittest.TestCase):
    def test_native_parser_raster_and_memory_boundaries(self):
        source = Path(__file__).resolve().parent
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("A host C compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "graphics-test"
            subprocess.run([
                compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(source.parent / "src"),
                "-I", str(source.parents[2] / "shared"),
                str(source / "test_graphics.c"), "-o", str(output),
            ], check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(output)], check=True, capture_output=True, timeout=30)
            self.assertIn(b"PASS WG1", result.stdout)
