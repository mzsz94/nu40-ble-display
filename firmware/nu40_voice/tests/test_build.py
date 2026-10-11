"""Factory package must preserve the MBR and the existing UF2 bootloader."""
from pathlib import Path
import struct
import tempfile
import unittest

from firmware.nu40_voice.build import provision_uf2

try:
    from intelhex import IntelHex
except ImportError:
    IntelHex = None


@unittest.skipIf(IntelHex is None, "Run in the Zephyr Python environment with intelhex")
class ProvisioningTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.boot = IntelHex()
        self.boot[0x1000] = 17
        self.boot.write_hex_file(self.root / "boot.hex")
        self.app = IntelHex()
        self.app[0x10000] = 23
        self.app[0x75fff] = 91
        self.app.write_hex_file(self.root / "app.hex")

    def test_sparse_confirmation_trailer_is_preserved_in_the_correct_page(self):
        data = provision_uf2(self.root / "boot.hex", self.root / "app.hex")
        self.assertEqual(len(data), 1536)
        headers = [struct.unpack_from("<8I", data, offset) for offset in range(0, len(data), 512)]
        self.assertEqual([header[3] for header in headers], [0x1000, 0x10000, 0x75f00])
        self.assertEqual(data[1024 + 32 + 255], 91)
        for index, header in enumerate(headers):
            self.assertEqual(header[2], 0x2000)
            self.assertEqual(header[4:], (256, index, 3, 0xADA52840))
            self.assertEqual(struct.unpack_from("<I", data, index * 512 + 508)[0], 0x0AB16F30)

    def test_reserved_regions_and_wrong_slot_are_rejected(self):
        for address in (0, 0xfff, 0x76000, 0xdc000, 0xe0000, 0x10001000):
            with self.subTest(address=address):
                image = IntelHex(self.app)
                image[address] = 0
                image.write_hex_file(self.root / "invalid.hex")
                with self.assertRaises(ValueError):
                    provision_uf2(self.root / "boot.hex", self.root / "invalid.hex")
        self.boot[0] = 1
        self.boot.write_hex_file(self.root / "invalid-boot.hex")
        with self.assertRaises(ValueError):
            provision_uf2(self.root / "invalid-boot.hex", self.root / "app.hex")


if __name__ == "__main__":
    unittest.main()
