"""Check real signatures, image corruption and protected-address bootstrap boundaries."""

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

import nu40_ota_package as package


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        cls.key = cls.root / "key.pem"
        cls.wrong_key = cls.root / "wrong.pem"
        for path in (cls.key, cls.wrong_key):
            path.write_bytes(ec.generate_private_key(ec.SECP256R1()).private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption()))
        raw = bytearray(b"\xff" * 1024)
        raw[:512] = b"\0" * 512
        struct.pack_into("<II", raw, 512, 0x20002000, package.SLOT_START + 513)
        (cls.root / "raw.bin").write_bytes(raw)
        cls.confirmed = cls.root / "confirmed.bin"
        subprocess.run([sys.executable, str(package.MCUBOOT / "scripts/imgtool.py"), "sign",
                        "--key", str(cls.key), "--version", "0.1.0", "--header-size", "512",
                        "--align", "4", "--slot-size", str(package.SLOT_SIZE),
                        "--max-sectors", "85", "--pad", "--confirm",
                        str(cls.root / "raw.bin"), str(cls.confirmed)],
                       check=True, capture_output=True, text=True, timeout=30)
        cls.boot = struct.pack("<II", 0x20002000, package.BOOT_START + 9) + b"\x00" * 64

    def test_actual_signature_verification_and_wrong_key_rejection(self):
        details = package.validate_signed_image(self.confirmed, self.key)
        self.assertEqual(details["version"], "0.1.0+0")
        with self.assertRaises(ValueError):
            package.validate_signed_image(self.confirmed, self.wrong_key)

    def test_corrupted_payload_or_hash_never_packages(self):
        original = self.confirmed.read_bytes()
        for offset in (0, 8, 12, 16, 512, 516, 900, 1028):
            data = bytearray(original)
            data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                package.bootstrap_bytes(self.boot, data)

    def test_initial_image_must_be_confirmed(self):
        data = bytearray(self.confirmed.read_bytes())
        data[-24] = 0xff
        with self.assertRaises(ValueError):
            package.bootstrap_bytes(self.boot, data)

    def test_recovery_regions_never_appear_in_uf2_and_secondary_is_empty(self):
        payload = package.bootstrap_bytes(self.boot, self.confirmed.read_bytes())
        flash = bytearray()
        for i in range(0, len(payload), 512):
            address, size = struct.unpack_from("<II", payload, i + 12)
            self.assertGreaterEqual(address, 0x1000)
            self.assertLessEqual(address + size, 0xC0000)
            flash.extend(payload[i + 32:i + 32 + size])
        self.assertEqual(flash[:len(self.boot)], self.boot)
        self.assertEqual(flash[package.SECONDARY_START - 0x1000:],
                         b"\xff" * (0xC0000 - package.SECONDARY_START))

    def test_second_stage_vectors_cannot_jump_into_factory_loader(self):
        for reset in (0, 0x1000, 0x10001, 0xe0001, 0x20001001):
            bad = struct.pack("<II", 0x20002000, reset) + b"\0" * 64
            with self.subTest(reset=reset), self.assertRaises(ValueError):
                package.bootstrap_bytes(bad, self.confirmed.read_bytes())


if __name__ == "__main__":
    unittest.main()
