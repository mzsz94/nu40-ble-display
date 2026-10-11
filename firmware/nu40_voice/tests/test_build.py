"""Factory package must preserve the MBR and the existing UF2 bootloader."""
from pathlib import Path
from contextlib import redirect_stderr
import io
import struct
import tempfile
import unittest

from firmware.nu40_voice.build import firmware_version, parse_args, provision_uf2, validate_version

try:
    from intelhex import IntelHex
except ImportError:
    IntelHex = None


class VersionTests(unittest.TestCase):
    arguments = ["--workspace", "/unused/workspace", "--key", "/unused/key.pem",
                 "--pairing-code-file", "/unused/pairing.txt"]

    def test_default_signing_version_follows_firmware_release(self):
        self.assertEqual(firmware_version(), "0.2.3")
        self.assertEqual(parse_args(self.arguments).version, firmware_version())

    def test_explicit_signing_version_override_and_header_limits(self):
        self.assertEqual(parse_args(self.arguments + ["--version", "1.2.3+17"]).version, "1.2.3+17")
        self.assertEqual(validate_version("255.255.65535+4294967295"), "255.255.65535+4294967295")
        for version in ("0.2", "0.2.1-rc1", "-1.2.3", "256.1.1", "1.256.1", "1.2.65536", "1.2.3+4294967296"):
            with self.subTest(version=version), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parse_args(self.arguments + ["--version", version])

    def test_version_file_tweak_and_invalid_fields(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "VERSION"
            source = "VERSION_MAJOR = 1\nVERSION_MINOR = 2\nPATCHLEVEL = 3\nVERSION_TWEAK = 4\nEXTRAVERSION =\n"
            path.write_text(source)
            self.assertEqual(firmware_version(path), "1.2.3+4")
            for content in (source.replace("VERSION_MINOR = 2\n", ""), source + "VERSION_MAJOR = 2\n",
                            source.replace("PATCHLEVEL = 3", "PATCHLEVEL = nope"),
                            source.replace("VERSION_MAJOR = 1", "VERSION_MAJOR = 256")):
                path.write_text(content)
                with self.assertRaises(ValueError):
                    firmware_version(path)


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
