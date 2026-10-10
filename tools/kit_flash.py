#!/usr/bin/env python3
"""Install a NU40 UF2 from a normal WSL terminal, then capture the selected app's logs."""

import argparse
import base64
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[1]
POWERSHELL = Path('/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
USBIPD = Path('/mnt/c/Program Files/usbipd-win/usbipd.exe')
FAMILY = 0xADA52840
TARGET_SERIAL = '87102B327CB78D38'  # NU40 connected to this hackathon PC.
SOFTWARE_BOOT_PRODUCTS = frozenset({'NU40DK KIT selftest v3', 'NU40DK BLE display v1',
                                   'NU40DK BLE display v2', 'NU40DK BLE display OTA v3'})
CAPTURE_MODES = {
    'kit': {'script': 'kit_selftest_capture.py', 'stem': 'kit-selftest',
            'evidence': 'kit-flash', 'label': 'KIT selftest', 'seconds': 60,
            'filename': 'KITTEST.UF2'},
    'ble-display': {'script': 'ble_display_capture.py', 'stem': 'ble-display',
                    'evidence': 'ble-display-flash', 'label': 'BLE display', 'seconds': 30,
                    'filename': 'BLEDISP.UF2'},
}


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--busid', default='1-2')
    parser.add_argument('--uf2', type=Path,
                        default=WORKSPACE / 'build/kit-selftest/zephyr/zephyr.uf2',
                        help='UF2 to install; the default remains the KIT selftest image.')
    parser.add_argument('--capture-mode', choices=CAPTURE_MODES, default='kit',
                        help='Select the log parser for the installed app (default: kit).')
    parser.add_argument('--capture-seconds', type=int,
                        help='Capture 20..300 seconds (default: kit 60, ble-display 30).')
    parser.add_argument('--serial-number', default=TARGET_SERIAL)
    return parser


def capture_command(mode, seconds, serial_number):
    return [sys.executable, str(WORKSPACE / 'tools' / CAPTURE_MODES[mode]['script']),
            '--seconds', str(seconds), '--serial-number', serial_number]


def archive_capture_evidence(evidence, stamp, mode):
    """Keep only the selected capture mode's previous logs with its flash metadata."""
    history = evidence / 'history'
    history.mkdir(exist_ok=True)
    stem = CAPTURE_MODES[mode]['stem']
    for previous in [evidence / 'metadata.json', WORKSPACE / f'build/{stem}.log',
                     WORKSPACE / f'build/{stem}.json']:
        if previous.is_file():
            shutil.copy2(previous, history / f'{stamp}-{previous.name}')


def validate_uf2(data):
    """Accept only a complete nRF52840 application starting at 0x1000."""
    if not data or len(data) % 512:
        raise ValueError('UF2 size must be a nonzero multiple of 512.')
    count = len(data) // 512
    addresses = []
    for number in range(count):
        block = data[number * 512:(number + 1) * 512]
        magic0, magic1, flags, address, size, index, total, family = struct.unpack(
            '<8I', block[:32]
        )
        end_magic, = struct.unpack('<I', block[508:])
        if (magic0, magic1, end_magic) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise ValueError('Invalid UF2 magic.')
        if flags != 0x2000 or family != FAMILY:
            raise ValueError('UF2 must contain only nRF52840 flash blocks.')
        if index != number or total != count or size != 256:
            raise ValueError('Incomplete or unsupported UF2 block sequence.')
        if address < 0x1000 or address + size > 0xC0000:
            raise ValueError('UF2 extends outside the allowed application region.')
        addresses.append(address)
    if addresses != list(range(0x1000, 0x1000 + count * 256, 256)):
        raise ValueError('Expected a contiguous application beginning at 0x1000.')
    return {'start': '0x1000', 'end': hex(addresses[-1] + 256), 'blocks': count,
            'sha256': hashlib.sha256(data).hexdigest()}


def validate_info(info):
    if not re.search(r'^Board-ID:\s*BARAM-NU40\s*$', info, re.MULTILINE):
        raise ValueError('This is not a BARAM-NU40 UF2 boot drive.')
    if not re.search(r'^SoftDevice:\s*not found\s*$', info, re.MULTILINE | re.IGNORECASE):
        raise ValueError('SoftDevice absence is not confirmed; refusing the 0x1000 image.')


def ps_literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def run(argv, timeout=20):
    result = subprocess.run(argv, capture_output=True, encoding='utf-8', errors='replace',
                            timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).strip() or str(argv))
    return result.stdout.strip().lstrip('\ufeff')


def powershell(script, timeout=20):
    prefix = ("$ErrorActionPreference='Stop'; "
              "[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding; ")
    encoded = base64.b64encode((prefix + script).encode('utf-16-le')).decode('ascii')
    return run([str(POWERSHELL), '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
               timeout=timeout)


def boot_drives():
    result = powershell("""
        $items = @();
        foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
            if ($drive.Name -notmatch '^[D-Z]$') { continue; }
            $root = $drive.Root;
            $file = $root + 'INFO_UF2.TXT';
            if (Test-Path -LiteralPath $file -PathType Leaf -ErrorAction SilentlyContinue) {
                $info = [System.IO.File]::ReadAllText($file);
                if ($info -match '(?m)^Board-ID:\\s*BARAM-NU40\\s*$') {
                    $items += @{root=$root; info=$info};
                }
            }
        }
        ConvertTo-Json -InputObject @($items) -Compress;
    """)
    return normalize_boot_drives(json.loads(result))


def normalize_boot_drives(value):
    """Handle PowerShell's extended string objects without assuming plain JSON strings."""
    if not isinstance(value, list):
        raise ValueError('Unexpected boot drive list from Windows PowerShell.')
    result = []
    for item in value:
        if not isinstance(item, dict) or not isinstance(item.get('root'), str):
            raise ValueError('Invalid boot drive entry from Windows PowerShell.')
        info = item.get('info')
        if isinstance(info, dict):
            info = info.get('value', info.get('Value'))
        if not isinstance(info, str):
            raise ValueError('Boot drive INFO content was not a string; no firmware was copied.')
        result.append({'root': item['root'], 'info': info})
    return result


def windows_path(path):
    return run(['wslpath', '-w', str(path.resolve())])


def request_software_boot(note, serial_number=TARGET_SERIAL):
    """Send uf2 only to exact known app identities that implement this command."""
    import serial
    from serial.tools import list_ports

    matches = [port for port in list_ports.comports()
               if port.vid == 0x2FE3 and port.pid == 0x0004]
    if (len(matches) != 1 or matches[0].product not in SOFTWARE_BOOT_PRODUCTS
            or matches[0].serial_number != serial_number):
        return False
    sent = False
    try:
        with serial.Serial(matches[0].device, 115200, timeout=0.1, write_timeout=2) as port:
            note(f'Requesting UF2 boot mode from {matches[0].product}.')
            sent = port.write(b'uf2\r\n') == 5
            port.flush()
    except (serial.SerialException, OSError) as error:
        note(f'Software boot request: {error}')
    return sent


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    capture = CAPTURE_MODES[args.capture_mode]
    if args.capture_seconds is None:
        args.capture_seconds = capture['seconds']
    if not re.fullmatch(r'\d+-\d+(?:\.\d+)*', args.busid):
        parser.error('Invalid USB bus ID.')
    if not 20 <= args.capture_seconds <= 300:
        parser.error('--capture-seconds must be between 20 and 300.')
    evidence = WORKSPACE / 'build' / capture['evidence']
    evidence.mkdir(parents=True, exist_ok=True)

    with (evidence / 'host.log').open('a', encoding='utf-8', buffering=1) as logfile:
        def note(message):
            print(message, flush=True)
            logfile.write(message + '\n')

        try:
            payload = args.uf2.read_bytes()
            image = validate_uf2(payload)
            if not POWERSHELL.exists() or not USBIPD.exists():
                raise RuntimeError('Windows PowerShell and usbipd-win are required.')
            if powershell("Write-Output 'READY'") != 'READY':
                raise RuntimeError('Windows PowerShell interop is unavailable.')
            note(f'{capture["label"]} UF2 verified: {image}')
            drives = boot_drives()
            if not drives:
                listing = run([str(USBIPD), 'list'])
                line = next((line for line in listing.splitlines()
                             if line.split() and line.split()[0] == args.busid), '')
                if '2fe3:0004' not in line.lower():
                    raise RuntimeError('Expected NU40 application USB device not found at bus ID.')
                software_boot = request_software_boot(note, args.serial_number)
                if software_boot:
                    time.sleep(1)
                    listing = run([str(USBIPD), 'list'])
                    line = next((line for line in listing.splitlines()
                                 if line.split() and line.split()[0] == args.busid), '')
                note('Releasing NU40 from WSL so Windows can access its boot drive.')
                if re.search(r'\bAttached\s*$', line):
                    run([str(USBIPD), 'detach', '--busid', args.busid])
                if software_boot:
                    note('Software boot requested; waiting for the boot drive.')
                else:
                    note('지금 NU40 모듈의 작은 RESET 버튼을 빠르게 두 번 눌러주세요.')
                    note('큰 KIT 키가 아닌 RESET입니다. 부트 드라이브를 최대 90초 기다립니다.')
                deadline = time.monotonic() + 90
                fallback_at = time.monotonic() + 10
                while time.monotonic() < deadline:
                    drives = boot_drives()
                    if drives:
                        break
                    if software_boot and time.monotonic() >= fallback_at:
                        note('부트 드라이브가 아직 없습니다. 작은 RESET 버튼을 두 번 눌러주세요.')
                        software_boot = False
                    time.sleep(2)
            if len(drives) != 1:
                raise RuntimeError('Expected exactly one NU40 boot drive; no firmware was copied.')
            drive = drives[0]
            info = drive['info'].replace('\r\n', '\n')
            (evidence / 'INFO_UF2.TXT').write_text(info, encoding='utf-8')
            note(info.strip())
            validate_info(info)

            # CURRENT.UF2 backs up the user application, not MBR/bootloader/UICR.
            stamp = time.strftime('%Y%m%d-%H%M%S')
            backup = evidence / f'CURRENT-{stamp}.UF2'
            backup_windows = windows_path(backup)
            root = ps_literal(drive['root'])
            powershell(f"Copy-Item -LiteralPath ({root} + 'CURRENT.UF2') "
                       f"-Destination {ps_literal(backup_windows)} -Force;")
            if not backup.is_file() or backup.stat().st_size < 512:
                raise RuntimeError('Application backup failed; no firmware was copied.')
            note(f'Existing application backup: {backup} ({backup.stat().st_size} bytes).')
            verified_payload = evidence / f'{capture["stem"]}-{image["sha256"][:12]}.uf2'
            verified_payload.write_bytes(payload)
            source = windows_path(verified_payload)
            metadata = {'uf2': str(args.uf2), **image, 'boot_info': info,
                        'application_backup': str(backup), 'flash_requested': True,
                        'capture_mode': args.capture_mode}
            archive_capture_evidence(evidence, stamp, args.capture_mode)
            (evidence / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
            note('부트 정보와 0x1000 호환성을 확인했습니다. '
                 f'{capture["label"]} 펌웨어를 복사합니다.')
            try:
                powershell(f"Copy-Item -LiteralPath {ps_literal(source)} "
                           f"-Destination ({root} + {ps_literal(capture['filename'])}) -Force;",
                           timeout=120)
                note('UF2 copy completed. Waiting for the NU40 application USB device.')
            except (RuntimeError, subprocess.SubprocessError) as copy_error:
                # UF2 can reboot the USB drive before Copy-Item returns. Only the
                # selected app's serial identity below can confirm a successful update.
                metadata['copy_error'] = str(copy_error)
                note(f'UF2 copy reported an error/disconnection: {copy_error}')
                note(f'Checking for {capture["label"]}; upload success is not yet confirmed.')
            (evidence / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
            attached = False
            for _ in range(15):
                time.sleep(2)
                try:
                    listing = run([str(USBIPD), 'list'])
                    line = next((line for line in listing.splitlines()
                                 if line.split() and line.split()[0] == args.busid), '')
                    if '2fe3:0004' not in line.lower():
                        continue
                    run([str(USBIPD), 'attach', '--wsl', '--busid', args.busid])
                    attached = True
                    break
                except RuntimeError:
                    continue
            if not attached:
                raise RuntimeError('UF2 copied, but the application could not be attached to WSL.')
            note(f'NU40 reattached. Starting the {capture["label"]} capture.')
            time.sleep(2)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError,
                KeyboardInterrupt) as error:
            note(f'STOP: {type(error).__name__}: {error}')
            note(f'Evidence saved in {evidence}. Share this output; do not repeat uploads blindly.')
            return 2

    return subprocess.call(capture_command(args.capture_mode, args.capture_seconds,
                                           args.serial_number))


if __name__ == '__main__':
    raise SystemExit(main())
