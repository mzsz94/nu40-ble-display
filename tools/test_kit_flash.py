"""Protect the application/bootloader boundary and the UF2 target identity."""

import contextlib
import io
import struct
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

import kit_flash


def block(address=0x1000, flags=0x2000, family=0xADA52840, index=0, count=1):
    header = struct.pack('<8I', 0x0A324655, 0x9E5D5157, flags, address,
                         256, index, count, family)
    return header + bytes(476) + struct.pack('<I', 0x0AB16F30)


class FlashValidationTests(unittest.TestCase):
    def test_default_mode_preserves_kit_image_and_capture(self):
        args = kit_flash.make_parser().parse_args([])
        self.assertEqual(args.capture_mode, 'kit')
        self.assertEqual(args.uf2,
                         kit_flash.WORKSPACE / 'build/kit-selftest/zephyr/zephyr.uf2')
        self.assertEqual(kit_flash.CAPTURE_MODES[args.capture_mode]['seconds'], 60)
        command = kit_flash.capture_command(args.capture_mode, 60, args.serial_number)
        self.assertEqual(command[1], str(kit_flash.WORKSPACE / 'tools/kit_selftest_capture.py'))
        self.assertEqual(command[2:], ['--seconds', '60', '--serial-number', args.serial_number])

    def test_ble_mode_dispatches_to_ble_parser_without_changing_default_image(self):
        args = kit_flash.make_parser().parse_args(['--capture-mode', 'ble-display'])
        self.assertEqual(args.uf2,
                         kit_flash.WORKSPACE / 'build/kit-selftest/zephyr/zephyr.uf2')
        self.assertEqual(kit_flash.CAPTURE_MODES[args.capture_mode]['seconds'], 30)
        command = kit_flash.capture_command(args.capture_mode, 30, 'target-serial')
        self.assertEqual(command[1], str(kit_flash.WORKSPACE / 'tools/ble_display_capture.py'))
        self.assertEqual(command[2:], ['--seconds', '30', '--serial-number', 'target-serial'])

    def test_ble_mode_accepts_explicit_image_and_capture_duration(self):
        args = kit_flash.make_parser().parse_args([
            '--capture-mode', 'ble-display', '--uf2', '/tmp/ble.uf2',
            '--capture-seconds', '90', '--serial-number', 'specific-board',
        ])
        self.assertEqual(args.uf2, Path('/tmp/ble.uf2'))
        self.assertEqual(args.capture_seconds, 90)
        self.assertEqual(args.serial_number, 'specific-board')

    def test_unknown_mode_and_invalid_durations_fail_before_hardware_access(self):
        for arguments in [['--capture-mode', 'unknown'],
                          ['--capture-mode', 'ble-display', '--capture-seconds', '19'],
                          ['--capture-mode', 'kit', '--capture-seconds', '301']]:
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()), \
                    patch.object(kit_flash, 'powershell') as ps, \
                    patch.object(kit_flash, 'request_software_boot') as request, \
                    self.assertRaises(SystemExit) as error:
                kit_flash.main(arguments)
            self.assertEqual(error.exception.code, 2)
            ps.assert_not_called()
            request.assert_not_called()

    def test_archiving_keeps_each_modes_logs_separate(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(kit_flash, 'WORKSPACE', Path(temporary)):
            workspace = Path(temporary)
            build = workspace / 'build'
            build.mkdir()
            for stem in ['kit-selftest', 'ble-display']:
                for extension in ['log', 'json']:
                    (build / f'{stem}.{extension}').write_text(f'{stem} {extension}')
            for mode, stem in [('kit', 'kit-selftest'), ('ble-display', 'ble-display')]:
                with self.subTest(mode=mode):
                    evidence = build / kit_flash.CAPTURE_MODES[mode]['evidence']
                    evidence.mkdir()
                    (evidence / 'metadata.json').write_text(mode)
                    kit_flash.archive_capture_evidence(evidence, 'saved', mode)
                    archived = evidence / 'history'
                    self.assertEqual({item.name for item in archived.iterdir()}, {
                        'saved-metadata.json', f'saved-{stem}.log', f'saved-{stem}.json',
                    })
                    self.assertEqual((archived / f'saved-{stem}.log').read_text(), f'{stem} log')
                    self.assertTrue((build / f'{stem}.log').is_file())

    def test_nrf_application_accepted(self):
        image = block(count=2) + block(0x1100, index=1, count=2)
        self.assertEqual(kit_flash.validate_uf2(image)['end'], '0x1200')

    def test_mbr_and_bootloader_images_rejected(self):
        for address in [0, 0xF4000, 0x100000]:
            with self.subTest(address=address), self.assertRaises(ValueError):
                kit_flash.validate_uf2(block(address))

    def test_other_family_rejected(self):
        with self.assertRaises(ValueError):
            kit_flash.validate_uf2(block(family=0xADA52800))

    def test_file_container_blocks_rejected(self):
        with self.assertRaises(ValueError):
            kit_flash.validate_uf2(block(flags=0x3000))

    def test_truncated_and_missing_blocks_rejected(self):
        for image in [b'', block()[:-1], block(count=2), block(index=1)]:
            with self.subTest(size=len(image)), self.assertRaises(ValueError):
                kit_flash.validate_uf2(image)

    def test_noncontiguous_blocks_rejected(self):
        image = block(count=2) + block(0x1200, index=1, count=2)
        with self.assertRaises(ValueError):
            kit_flash.validate_uf2(image)

    def test_correct_info(self):
        kit_flash.validate_info('Board-ID: BARAM-NU40\nSoftDevice: not found\n')

    def test_softdevice_unknown_or_present_rejected(self):
        for detail in ['', 'SoftDevice: S140 6.1.1', 'SoftDevice: unknown']:
            with self.subTest(detail=detail), self.assertRaises(ValueError):
                kit_flash.validate_info('Board-ID: BARAM-NU40\n' + detail)

    def test_other_board_and_substring_rejected(self):
        for board in ['NU40', 'BARAM-NU40-OTHER', 'BARAM-NU32']:
            with self.subTest(board=board), self.assertRaises(ValueError):
                kit_flash.validate_info(f'Board-ID: {board}\nSoftDevice: not found\n')

    def test_powershell_literal_quotes(self):
        self.assertEqual(kit_flash.ps_literal("E:\\it's fine"), "'E:\\it''s fine'")

    def test_large_bootstrap_copy_can_use_extended_timeout(self):
        with patch.object(kit_flash, 'run', return_value='copied') as run:
            self.assertEqual(kit_flash.powershell('Write-Output copied', timeout=120), 'copied')
        self.assertEqual(run.call_args.kwargs['timeout'], 120)

    def test_discovery_emits_valid_script_shape(self):
        with patch.object(kit_flash, 'powershell', return_value='[]') as ps:
            self.assertEqual(kit_flash.boot_drives(), [])
        script = ps.call_args.args[0]
        self.assertTrue(script.lstrip().startswith('$items = @();'))
        self.assertIn('$root = $drive.Root;', script)
        self.assertIn(r'BARAM-NU40\s*$', script)
        self.assertIn('[System.IO.File]::ReadAllText($file)', script)

    def test_powershell_extended_string_is_normalized(self):
        info = 'Board-ID: BARAM-NU40\r\nSoftDevice: not found\r\n'
        raw = [{'root': 'E:\\', 'info': {'value': info, 'PSPath': 'E:\\INFO_UF2.TXT'}}]
        normalized = kit_flash.normalize_boot_drives(raw)
        self.assertEqual(normalized, [{'root': 'E:\\', 'info': info}])
        kit_flash.validate_info(normalized[0]['info'])

    def test_malformed_info_stops_before_copy(self):
        for value in [None, {}, [{'root': 'E:\\', 'info': {'PSPath': 'unknown'}}]]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                kit_flash.normalize_boot_drives(value)

    def test_software_boot_does_not_send_to_v2_or_unknown_devices(self):
        import serial
        from serial.tools import list_ports

        for product in ['NU40DK KIT selftest', 'Unknown', 'NU40DK KIT selftest v30',
                        'NU40DK BLE display', 'NU40DK BLE display v10']:
            port = types.SimpleNamespace(device='/dev/ttyACM0', vid=0x2FE3, pid=4,
                                         product=product)
            with self.subTest(product=product), patch.object(
                    list_ports, 'comports', return_value=[port]), patch.object(
                    serial, 'Serial') as open_port:
                self.assertFalse(kit_flash.request_software_boot(lambda message: None))
                open_port.assert_not_called()

    def test_v3_software_boot_sends_only_uf2_at_normal_baud(self):
        import serial
        from serial.tools import list_ports

        port = types.SimpleNamespace(device='/dev/ttyACM1', vid=0x2FE3, pid=4,
                                     product='NU40DK KIT selftest v3',
                                     serial_number=kit_flash.TARGET_SERIAL)
        with patch.object(list_ports, 'comports', return_value=[port]), patch.object(
                serial, 'Serial') as open_port:
            connection = open_port.return_value.__enter__.return_value
            connection.write.return_value = 5
            self.assertTrue(kit_flash.request_software_boot(lambda message: None))
            connection.write.assert_called_once_with(b'uf2\r\n')
            open_port.assert_called_once_with('/dev/ttyACM1', 115200,
                                             timeout=0.1, write_timeout=2)

    def test_ble_v1_software_boot_sends_only_uf2_to_exact_target(self):
        import serial
        from serial.tools import list_ports

        port = types.SimpleNamespace(device='/dev/ttyACM2', vid=0x2FE3, pid=4,
                                     product='NU40DK BLE display v1', serial_number='ble-board')
        with patch.object(list_ports, 'comports', return_value=[port]), patch.object(
                serial, 'Serial') as open_port:
            connection = open_port.return_value.__enter__.return_value
            connection.write.return_value = 5
            self.assertTrue(kit_flash.request_software_boot(lambda message: None, 'ble-board'))
            connection.write.assert_called_once_with(b'uf2\r\n')
            open_port.assert_called_once_with('/dev/ttyACM2', 115200,
                                             timeout=0.1, write_timeout=2)

    def test_ble_boot_rejects_wrong_usb_identity_or_serial(self):
        import serial
        from serial.tools import list_ports

        for vid, pid, serial_number in [(0x10C4, 4, kit_flash.TARGET_SERIAL),
                                        (0x2FE3, 5, kit_flash.TARGET_SERIAL),
                                        (0x2FE3, 4, 'another-board')]:
            port = types.SimpleNamespace(device='/dev/ttyACM2', vid=vid, pid=pid,
                                         product='NU40DK BLE display v1',
                                         serial_number=serial_number)
            with self.subTest(vid=vid, pid=pid, serial=serial_number), patch.object(
                    list_ports, 'comports', return_value=[port]), patch.object(
                    serial, 'Serial') as open_port:
                self.assertFalse(kit_flash.request_software_boot(lambda message: None))
                open_port.assert_not_called()

    def test_software_boot_rejects_wrong_serial_and_multiple_nu40_ports(self):
        import serial
        from serial.tools import list_ports

        target = types.SimpleNamespace(device='/dev/ttyACM0', vid=0x2FE3, pid=4,
                                       product='NU40DK KIT selftest v3',
                                       serial_number=kit_flash.TARGET_SERIAL)
        other = types.SimpleNamespace(device='/dev/ttyACM1', vid=0x2FE3, pid=4,
                                      product='NU40DK KIT selftest v3',
                                      serial_number='other-board')
        for ports in [[other], [target, other]]:
            with self.subTest(ports=ports), patch.object(
                    list_ports, 'comports', return_value=ports), patch.object(
                    serial, 'Serial') as open_port:
                self.assertFalse(kit_flash.request_software_boot(lambda message: None))
                open_port.assert_not_called()


if __name__ == '__main__':
    unittest.main()
