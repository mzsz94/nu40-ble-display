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
        count = (0xe0000 - 0x1000) // 256
        self.assertEqual(len(data), count * 512)
        headers = [struct.unpack_from("<8I", data, offset) for offset in range(0, len(data), 512)]
        self.assertEqual([header[3] for header in headers], list(range(0x1000, 0xe0000, 256)))
        trailer_index = (0x75f00 - 0x1000) // 256
        self.assertEqual(data[trailer_index * 512 + 32 + 255], 91)
        for index, header in enumerate(headers):
            self.assertEqual(header[:2], (0x0A324655, 0x9E5D5157))
            self.assertEqual(header[2], 0x2000)
            self.assertEqual(header[4:], (256, index, count, 0xADA52840))
            self.assertEqual(struct.unpack_from("<I", data, index * 512 + 508)[0], 0x0AB16F30)

    def test_factory_migration_clears_old_swap_state_and_bonds_preserving_boot_regions(self):
        # Simulate a different previous image/layout with non-erased status,
        # a pending secondary image and old keys. UF2 changes payload bytes
        # only; untouched bytes in a physical page survive its flash cache.
        previous = bytes([0x5a]) * 0x100000
        flash = bytearray(previous)
        data = provision_uf2(self.root / "boot.hex", self.root / "app.hex")
        for offset in range(0, len(data), 512):
            header = struct.unpack_from("<8I", data, offset)
            address, size = header[3:5]
            flash[address:address + size] = data[offset + 32:offset + 32 + size]
        self.assertEqual(flash[:0x1000], previous[:0x1000])
        self.assertEqual(flash[0xe0000:], previous[0xe0000:])
        self.assertEqual(flash[0x75000:0x75fff], b"\xff" * 0xfff)
        self.assertEqual(flash[0x75fff], 91)
        self.assertEqual(flash[0x76000:0xdc000], b"\xff" * 0x66000)
        self.assertEqual(flash[0xdc000:0xe0000], b"\xff" * 0x4000)
        self.assertEqual(flash[0x1000], 17)
        self.assertEqual(flash[0x10000], 23)
        for address in (0x8000, 0x20000, 0x74000):
            self.assertEqual(flash[address:address + 4096], b"\xff" * 4096)

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
