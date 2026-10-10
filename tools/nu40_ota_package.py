#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate signed NU40 images and package an application-region-only UF2 bootstrap."""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

from kit_flash import validate_uf2

ROOT = Path(__file__).resolve().parents[1]
MCUBOOT = Path(os.environ.get("MCUBOOT_DIR", ROOT.parent / "bootloader/mcuboot")).expanduser()
BOOT_START, BOOT_END = 0x1000, 0x10000
SLOT_START, SLOT_SIZE = 0x10000, 0x55000
SECONDARY_START, SCRATCH_START, STORAGE_START, LIMIT = 0x65000, 0xBA000, 0xBE000, 0xC0000
MAGIC = 0x96F3B83D
TRAILER_MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")
DEFAULT_KEY = ROOT / "build/ota-keys/nu40-ec-p256.pem"


def check_vector(data, start, end):
    if len(data) < 8:
        raise ValueError("Missing Cortex-M vector table")
    stack, reset = struct.unpack_from("<II", data)
    if not 0x20000000 < stack <= 0x20040000 or stack % 8 or not reset & 1:
        raise ValueError("Invalid Cortex-M stack/reset vector")
    if not start <= reset & ~1 < end:
        raise ValueError("Reset vector leaves its allowed flash region")


def image_details(data):
    if not 512 + 8 <= len(data) <= SLOT_SIZE:
        raise ValueError("Image size is outside the primary/secondary slot")
    magic, load, header, protected, size, flags = struct.unpack_from("<IIHHII", data)
    if magic != MAGIC or load != 0 or header != 512 or flags != 0 or size < 8:
        raise ValueError("Expected an unencrypted, executable NU40 MCUboot image")
    end = header + size + protected
    if end + 4 > len(data):
        raise ValueError("Truncated image")
    check_vector(data[header:], SLOT_START + header, SLOT_START + header + size)
    if protected:
        if protected < 4 or struct.unpack_from("<HH", data, header + size) != (0x6908, protected):
            raise ValueError("Invalid protected TLV area")
    tlv_magic, total = struct.unpack_from("<HH", data, end)
    if tlv_magic != 0x6907 or total < 4 or end + total > len(data):
        raise ValueError("Invalid signature TLV area")
    digest = None
    offset = end + 4
    while offset < end + total:
        if offset + 4 > end + total:
            raise ValueError("Truncated TLV header")
        kind, reserved, length = struct.unpack_from("<BBH", data, offset)
        offset += 4
        if reserved != 0 or offset + length > end + total:
            raise ValueError("Truncated or invalid TLV")
        if kind == 0x10:
            if digest is not None or length != 32:
                raise ValueError("Invalid SHA256 TLV")
            digest = data[offset:offset + length]
        offset += length
    if digest != hashlib.sha256(data[:end]).digest():
        raise ValueError("Image SHA256 mismatch")
    version = struct.unpack_from("<BBHI", data, 20)
    return {"version": f"{version[0]}.{version[1]}.{version[2]}+{version[3]}",
            "image_hash": digest.hex(), "file_sha256": hashlib.sha256(data).hexdigest(),
            "bytes": len(data), "signed_end": end + total}


def validate_signed_image(path, key=DEFAULT_KEY):
    details = image_details(path.read_bytes())
    sys.path.insert(0, str(MCUBOOT / "scripts"))
    from imgtool import keys
    from imgtool.image import Image, VerifyResult

    loaded_key = keys.load(str(key))
    if loaded_key is None:
        raise ValueError("Signing verification key could not be loaded")
    result, _version, digest, *_rest = Image.verify(str(path), loaded_key)
    if result != VerifyResult.OK or digest.hex() != details["image_hash"]:
        raise ValueError(f"MCUboot signature verification failed: {result}")
    return details


def bootstrap_bytes(boot, confirmed):
    if not 8 <= len(boot) <= BOOT_END - BOOT_START:
        raise ValueError("MCUboot exceeds its second-stage partition")
    check_vector(boot, BOOT_START, BOOT_START + len(boot))
    image_details(confirmed)
    if len(confirmed) != SLOT_SIZE or confirmed[-16:] != TRAILER_MAGIC:
        raise ValueError("Initial image must be padded, signed and confirmed")
    # imgtool writes the 4-byte-aligned image_ok flag before its final 16-byte magic.
    if confirmed[-24] != 1:
        raise ValueError("Initial image is not marked confirmed")
    flash = bytearray(b"\xff" * (LIMIT - BOOT_START))
    flash[:len(boot)] = boot
    app_offset = SLOT_START - BOOT_START
    flash[app_offset:app_offset + len(confirmed)] = confirmed
    count = len(flash) // 256
    result = bytearray()
    for i in range(count):
        block = bytearray(512)
        struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157, 0x2000,
                         BOOT_START + i * 256, 256, i, count, 0xADA52840)
        block[32:288] = flash[i * 256:(i + 1) * 256]
        struct.pack_into("<I", block, 508, 0x0AB16F30)
        result.extend(block)
    validate_uf2(result)
    return bytes(result)


def check_builds(boot_dir, app_dir, key):
    for folder, required in (
        (boot_dir, ['CONFIG_USE_DT_CODE_PARTITION=y', 'CONFIG_BOOT_SWAP_USING_SCRATCH=y',
                    'CONFIG_MAIN_STACK_SIZE=10240',
                    'CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y',
                    f'CONFIG_BOOT_SIGNATURE_KEY_FILE="{key.resolve()}"']),
        (app_dir, ['CONFIG_BOOTLOADER_MCUBOOT=y', 'CONFIG_USE_DT_CODE_PARTITION=y',
                   'CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_SCRATCH=y',
                   'CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY=y',
                   f'CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="{key.resolve()}"'])):
        config = (folder / "zephyr/.config").read_text().splitlines()
        if any(line not in config for line in required):
            raise ValueError(f"Incompatible signing/boot configuration: {folder}")
        dts = (folder / "zephyr/zephyr.dts").read_text()
        for address, size in ((BOOT_START, BOOT_END - BOOT_START), (SLOT_START, SLOT_SIZE),
                              (SECONDARY_START, SLOT_SIZE), (SCRATCH_START, 0x4000),
                              (STORAGE_START, 0x2000)):
            if f"reg = < 0x{address:x} 0x{size:x} >" not in dts:
                raise ValueError(f"Unexpected flash partitions in {folder}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-build", type=Path, default=ROOT / "build/nu40-ota-boot")
    parser.add_argument("--app-build", type=Path, default=ROOT / "build/nu40-ota-base")
    parser.add_argument("--key", type=Path, default=DEFAULT_KEY)
    parser.add_argument("--output", type=Path, default=ROOT / "build/nu40-ota-bootstrap")
    args = parser.parse_args(argv)
    check_builds(args.boot_build, args.app_build, args.key)
    args.output.mkdir(parents=True, exist_ok=True)
    signed = args.app_build / "zephyr/zephyr.signed.bin"
    details = validate_signed_image(signed, args.key)
    confirmed = args.output / "initial.signed.confirmed.bin"
    subprocess.run([sys.executable, str(MCUBOOT / "scripts/imgtool.py"), "sign", "--key",
                    str(args.key), "--version", details["version"], "--header-size", "512",
                    "--align", "4", "--slot-size", str(SLOT_SIZE), "--max-sectors", "85",
                    "--pad", "--confirm", str(args.app_build / "zephyr/zephyr.bin"),
                    str(confirmed)], check=True)
    initial = validate_signed_image(confirmed, args.key)
    if initial["image_hash"] != details["image_hash"]:
        raise ValueError("Confirmed bootstrap does not match the verified application")
    boot = (args.boot_build / "zephyr/zephyr.bin").read_bytes()
    payload = bootstrap_bytes(boot, confirmed.read_bytes())
    target = args.output / "bootstrap.uf2"
    target.write_bytes(payload)
    manifest = {"uf2": validate_uf2(payload), "initial_image": initial,
                "boot_sha256": hashlib.sha256(boot).hexdigest(),
                "partitions": {"boot": [BOOT_START, BOOT_END],
                               "primary": [SLOT_START, SLOT_START + SLOT_SIZE],
                               "secondary": [SECONDARY_START, SECONDARY_START + SLOT_SIZE],
                               "scratch": [SCRATCH_START, STORAGE_START],
                               "storage": [STORAGE_START, LIMIT]},
                "preserved": "[0,0x1000) MBR and [0xc0000,0x100000) factory UF2/settings",
                "flash_performed": False}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
