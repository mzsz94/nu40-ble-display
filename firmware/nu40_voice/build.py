#!/usr/bin/env python3
"""Build signed NU40 firmware and initial provisioning UF2. Never flashes hardware."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).resolve().parent
FAMILY_NRF52840 = 0xADA52840
SLOT_SIZE = 0x66000
FACTORY_START = 0x1000
FACTORY_END = 0xE0000


def private_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(data)


def provision_uf2(boot_hex: Path, confirmed_hex: Path) -> bytes:
    from intelhex import IntelHex
    images = [(IntelHex(str(boot_hex)), 0x1000, 0x10000),
              (IntelHex(str(confirmed_hex)), 0x10000, 0x76000)]
    # This is a factory migration, including devices with another MCUboot
    # layout. UF2 preserves old bytes not explicitly present in a payload,
    # even when it erases/reprograms the surrounding physical flash page.
    # Emit every byte in our firmware partitions to clear stale primary swap
    # status, secondary images/trailers and NVS bonds before the first boot.
    # Keep the existing MBR and onboard UF2 bootloader completely untouched.
    flash = bytearray(b"\xff" * (FACTORY_END - FACTORY_START))
    for image, low, high in images:
        if not image.addresses() or any(address < low or address >= high for address in image.addresses()):
            raise ValueError("Provisioning image exceeds its board partition; UF2/MBR region must be preserved")
        for address in image.addresses():
            flash[address - FACTORY_START] = image[address]
    output = bytearray()
    count = len(flash) // 256
    for number in range(count):
        address = FACTORY_START + number * 256
        payload = flash[number * 256:(number + 1) * 256]
        header = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, address, 256, number, count, FAMILY_NRF52840)
        output.extend(header + payload + bytes(220) + struct.pack("<I", 0x0AB16F30))
    return bytes(output)


def validate_version(version: str) -> str:
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)(?:\+([0-9]+))?", version)
    if not match:
        raise ValueError("version must be major.minor.patch[+build]")
    parts = [int(value or "0") for value in match.groups()]
    if any(value > limit for value, limit in zip(parts, (255, 255, 65535, 0xffffffff))):
        raise ValueError("version exceeds the MCUboot image header field limits")
    return version


def firmware_version(path: Path = SOURCE / "VERSION") -> str:
    """Use the same numeric version as Zephyr instead of a second release default."""
    required = ("VERSION_MAJOR", "VERSION_MINOR", "PATCHLEVEL", "VERSION_TWEAK")
    values = {}
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, separator, value = line.partition("=")
        name, value = name.strip(), value.strip()
        if name not in required:
            continue
        if not separator or name in values or not re.fullmatch(r"[0-9]+", value):
            raise ValueError("VERSION contains an invalid or duplicate numeric field")
        values[name] = int(value)
    if any(name not in values for name in required):
        raise ValueError("VERSION is missing a required numeric field")
    version = ".".join(str(values[name]) for name in required[:3])
    if values["VERSION_TWEAK"]:
        version += f'+{values["VERSION_TWEAK"]}'
    return validate_version(version)


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True, help="west workspace containing zephyr and bootloader/mcuboot")
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/nu40-voice")
    parser.add_argument("--key", type=Path, required=True, help="private persistent P256 signing key; never commit")
    parser.add_argument("--pairing-code-file", type=Path, required=True, help="private six-digit BLE code shared with NU87")
    parser.add_argument("--create-secrets", action="store_true", help="create missing private key/code once; preserve existing secrets")
    parser.add_argument("--version", help="override the signed image version; defaults to this firmware's VERSION file")
    parser.add_argument("--overlay", type=Path, help="additional verified hardware pin overlay")
    return parser


def parse_args(argv: list[str] | None = None, *, parser: argparse.ArgumentParser | None = None) -> argparse.Namespace:
    parser = parser if parser is not None else argument_parser()
    args = parser.parse_args(argv)
    try:
        args.version = firmware_version() if args.version is None else validate_version(args.version)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return args


def main() -> int:
    # Compiled firmware embeds the locally provisioned BLE pairing code.
    # Build products and manifests therefore receive private file permissions.
    os.umask(0o077)
    parser = argument_parser()
    args = parse_args(parser=parser)
    workspace, build, key, pairing = (p.expanduser().resolve() for p in (args.workspace, args.build_dir, args.key, args.pairing_code_file))
    # Keep the virtualenv entry path; resolving its symlink loses site-packages.
    python = args.python.expanduser().absolute()
    zephyr = workspace / "zephyr"
    imgtool = workspace / "bootloader/mcuboot/scripts/imgtool.py"
    if not zephyr.is_dir() or not imgtool.is_file():
        parser.error("workspace must include Zephyr and MCUboot sources")
    if key.is_relative_to(build) or pairing.is_relative_to(build):
        parser.error("Keep secrets outside build-dir: pristine builds replace that directory")
    build.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(build, 0o700)
    if not key.exists() and args.create_secrets:
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives import serialization
        private_write(key, ec.generate_private_key(ec.SECP256R1()).private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    if not pairing.exists() and args.create_secrets:
        private_write(pairing, f"{secrets.randbelow(1000000):06d}\n".encode())
    if not key.is_file() or not pairing.is_file():
        parser.error("Private key/code missing; use --create-secrets once or provide existing files")
    code = pairing.read_text().strip()
    if not re.fullmatch(r"[0-9]{6}", code):
        parser.error("pairing code file must contain exactly six decimal digits")
    # Check type before invoking the build, without printing key material.
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    signing_key = serialization.load_pem_private_key(key.read_bytes(), password=None)
    if not isinstance(signing_key, ec.EllipticCurvePrivateKey) or not isinstance(signing_key.curve, ec.SECP256R1):
        parser.error("signing key must be an ECDSA P256 private key")
    if key.stat().st_mode & 0o077 or pairing.stat().st_mode & 0o077:
        parser.error("Set private key/code file permissions to 600 before building")
    local_config = key.parent / "nu40-private.conf"
    config_text = f'CONFIG_WALLABY_PAIRING_PASSKEY={int(code)}\nCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="{args.version}"\n'
    descriptor = os.open(local_config, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as output:
        output.write(config_text)
    os.chmod(local_config, 0o600)
    env = dict(os.environ, ZEPHYR_BASE=str(zephyr), ZEPHYR_TOOLCHAIN_VARIANT="zephyr")
    env["PATH"] = str(python.parent) + os.pathsep + env.get("PATH", "")
    if args.sdk:
        env["ZEPHYR_SDK_INSTALL_DIR"] = str(args.sdk.expanduser().resolve())
    command = [str(python), "-m", "west", "build", "--sysbuild", "-p", "always", "-b", "nucode_nu40/nrf52840", str(SOURCE), "-d", str(build), "--",
               f"-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE={json.dumps(str(key))}", f"-Dnu40_voice_EXTRA_CONF_FILE={local_config}"]
    if args.overlay:
        command.append(f"-Dnu40_voice_EXTRA_DTC_OVERLAY_FILE={args.overlay.expanduser().resolve()}")
    subprocess.run(command, cwd=workspace, env=env, check=True)
    app = build / "nu40_voice/zephyr"
    boot = build / "mcuboot/zephyr/zephyr.hex"
    update = app / "zephyr.signed.bin"
    if update.stat().st_size > SLOT_SIZE - 0x2000:
        raise ValueError("Signed update exceeds swap-move image capacity (two pages reserved)")
    subprocess.run([str(python), str(imgtool), "verify", "-k", str(key), str(update)], check=True, env=env)
    artifacts = build / "artifacts"
    artifacts.mkdir(exist_ok=True, mode=0o700)
    os.chmod(artifacts, 0o700)
    provision = artifacts / "initial-provision.uf2"
    provision.write_bytes(provision_uf2(boot, app / "zephyr.signed.confirmed.hex"))
    shutil.copyfile(update, artifacts / "nu40-update.signed.bin")
    manifest = {"project": "military-wallaby", "version": args.version,
                "transport": "BLE SMP authenticated L3", "signature": "ECDSA-P256",
                "test_requires_explicit_host_confirmation": True,
                "initial_provision_requires_usb": True,
                "initial_provision_flash_range": {"start": "0x1000", "end_exclusive": "0xe0000"},
                "initial_provision_resets": ["previous firmware and MCUboot swap metadata", "secondary image slot", "NVS settings and BLE bonds"],
                "files": {name: {"bytes": (artifacts / name).stat().st_size,
                                   "sha256": hashlib.sha256((artifacts / name).read_bytes()).hexdigest()}
                          for name in ("initial-provision.uf2", "nu40-update.signed.bin")}}
    (artifacts / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for path in (provision, artifacts / "nu40-update.signed.bin", artifacts / "manifest.json"):
        os.chmod(path, 0o600)
    print(f"Built and signature-verified: {artifacts}")
    print("No device was flashed. Preserve the private signing key and shared BLE pairing file.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Build failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise SystemExit(1)
