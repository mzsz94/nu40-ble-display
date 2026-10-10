"""Exercise SMP correlation and the upload/trial/confirmation sequence without hardware."""

import copy
import hashlib
import io
import unittest
from unittest.mock import Mock, patch

import cbor2

import nu87_ota as ota

OLD_HASH = bytes.fromhex("11" * 32)
NEW_HASH = bytes.fromhex("22" * 32)
IMAGE = bytes(range(256)) * 2
DETAILS = {"version": "0.1.1+0", "image_hash": NEW_HASH.hex()}


def state(slot=0, digest=OLD_HASH, version="0.1.0+0", **flags):
    return {"slot": slot, "hash": digest, "version": version,
            "active": slot == 0, "confirmed": slot == 0, **flags}


class FakeRelay:
    def __init__(self):
        self.calls, self.modes = [], []
        self.images = [state()]
        self.uploaded = bytearray()
        self.reset_count = 0
        self.reset_timeout = False
        self.offset_override = None

    def mode(self, command):
        self.modes.append(command)

    def ready(self, timeout=40):
        return None

    def request(self, op, group, command, values):
        self.calls.append((op, group, command, copy.deepcopy(values)))
        if (op, group, command) == (0, 1, 0):
            return {"images": copy.deepcopy(self.images)}
        if (op, group, command) == (2, 1, 1):
            if self.offset_override is not None:
                return {"off": self.offset_override}
            offset = values["off"]
            if offset == 0:
                assert values["len"] == len(IMAGE)
                assert values["sha"] == hashlib.sha256(IMAGE).digest()
                assert values["upgrade"] is True
            assert offset == len(self.uploaded)
            self.uploaded.extend(values["data"])
            if len(self.uploaded) == len(IMAGE):
                self.images.append(state(1, NEW_HASH, DETAILS["version"], confirmed=False))
            return {"off": len(self.uploaded)}
        if (op, group, command) == (2, 1, 0):
            assert values == {"hash": NEW_HASH, "confirm": False}
            self.images[1]["pending"] = True
            return {"images": copy.deepcopy(self.images)}
        if (op, group, command) == (2, 0, 5):
            self.reset_count += 1
            self.images = [state(0, NEW_HASH, DETAILS["version"]), state(1)]
            if self.reset_timeout:
                raise TimeoutError("reset disconnected before reply")
            return {}
        raise AssertionError("Unexpected remote request")


class SMPTests(unittest.TestCase):
    def setUp(self):
        self.request = ota.make_request(0, 1, 27, 0, {})

    def response(self, value, **changes):
        fields = dict(op=1, flags=0, length=0, group=1, sequence=27, command=0)
        data = cbor2.dumps(value)
        fields["length"] = len(data)
        fields.update(changes)
        return ota.HEADER.pack(*fields.values()) + data

    def test_exact_response_and_byte_hash_survive_cbor(self):
        value = {"images": [state()]}
        self.assertEqual(ota.decode_response(self.request, self.response(value)), value)

    def test_zephyr_and_imgtool_version_notations_compare_equally(self):
        for first, second in (("0.1.1", "0.1.1+0"), ("0.1.1.5", "0.1.1+5")):
            self.assertEqual(ota.version_components(first), ota.version_components(second))
        for value in ("unknown", "0.1", "256.1.1", "0.1.65536", "0.1.1+4294967296"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                ota.version_components(value)

    def test_wrong_transaction_headers_and_lengths_are_rejected(self):
        for change in ({"op": 3}, {"flags": 1}, {"group": 0}, {"sequence": 28},
                       {"command": 1}, {"length": 700}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                ota.decode_response(self.request, self.response({}, **change))
        for packet in (b"", self.response({})[:-1], b"x" * 513):
            with self.subTest(size=len(packet)), self.assertRaises(ValueError):
                ota.decode_response(self.request, packet)

    def test_cbor_must_be_one_map_and_error_codes_are_enforced(self):
        for value in ([], 5, {"err": "oops"}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                ota.decode_response(self.request, self.response(value))
        for value in ({"rc": 5}, {"err": {"group": 1, "rc": 2}}):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                ota.decode_response(self.request, self.response(value))
        data = cbor2.dumps({}) + cbor2.dumps({})
        packet = ota.HEADER.pack(1, 0, len(data), 1, 27, 0) + data
        with self.assertRaises(ValueError):
            ota.decode_response(self.request, packet)

    def test_ambiguous_or_inactive_slot_lists_are_rejected(self):
        for images in ([state(), state()], [state(active=False)], [state(slot=2)],
                       [state(digest=b"bad")], [state(image=1)], [state(version=1)]):
            with self.subTest(images=images), self.assertRaises(ValueError):
                ota.image_states({"images": images})

    def test_pause_is_acknowledged_before_large_serial_requests(self):
        relay = ota.Relay(None, io.StringIO())
        with patch.object(relay, "command") as command, patch.object(
                relay, "line", side_effect=["NU87|SEND result=0", "NU87|MODE value=pause"]):
            relay.mode("pause")
            command.assert_called_once_with("pause")

    def test_uart_pacing_stays_below_sdk_ring_capacity(self):
        port = Mock()
        relay = ota.Relay(port, io.StringIO())
        value = "smp " + "a5" * 512
        with patch.object(ota.time, "sleep") as sleep:
            relay.command(value)
        pieces = [call.args[0] for call in port.write.call_args_list]
        self.assertEqual(b"".join(pieces), (value + "\n").encode())
        self.assertLessEqual(max(map(len, pieces)), 32)
        self.assertEqual(port.flush.call_count, len(pieces))
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [0.02] * len(pieces))

    def test_missing_pause_ack_aborts_before_upload(self):
        relay = ota.Relay(None, io.StringIO())
        with patch.object(relay, "command"), patch.object(
                relay, "line", side_effect=TimeoutError), self.assertRaises(TimeoutError):
            relay.mode("pause")

    def test_ready_retries_after_a_boot_drops_the_first_uart_query(self):
        relay = ota.Relay(None, io.StringIO())
        clock = iter(range(100))
        with patch.object(ota.time, "monotonic", side_effect=lambda: next(clock)), \
                patch.object(relay, "command") as command, patch.object(
                    relay, "line", side_effect=[TimeoutError, "NU87|BOOT firmware=3",
                                                "NU87|RELAY firmware=3 smp_ready=1 paused=0"]):
            relay.ready(timeout=40)
        self.assertGreaterEqual(command.call_count, 2)


class UploadTests(unittest.TestCase):
    def run_upload(self, relay):
        report = {}
        ota.upload(relay, IMAGE, DETAILS, report, progress=lambda value: None)
        return report

    def test_full_upload_checks_secondary_schedules_trial_and_confirms_primary(self):
        relay = FakeRelay()
        report = self.run_upload(relay)
        self.assertEqual(relay.uploaded, IMAGE)
        self.assertEqual(relay.reset_count, 1)
        self.assertEqual(relay.modes, ["pause", "face"])
        self.assertTrue(report["trial_scheduled"])
        self.assertEqual(report["result"], "installed_and_confirmed")
        self.assertEqual(report["after"][0]["hash"], NEW_HASH.hex())

    def test_reset_reply_loss_does_not_trigger_second_reset(self):
        relay = FakeRelay()
        relay.reset_timeout = True
        report = self.run_upload(relay)
        self.assertEqual(relay.reset_count, 1)
        self.assertEqual(report["result"], "installed_and_confirmed")
        self.assertIn("reset_response_error", report)

    def test_upload_accepts_version_without_zero_build_suffix(self):
        relay = FakeRelay()
        original = relay.request

        def zephyr_reply(*args):
            reply = original(*args)
            for image in reply.get("images", []):
                image["version"] = image["version"].removesuffix("+0")
            return reply

        with patch.object(relay, "request", side_effect=zephyr_reply):
            self.assertEqual(self.run_upload(relay)["result"], "installed_and_confirmed")

    def test_fresh_session_verifies_installed_image_without_upload_or_reset(self):
        relay = FakeRelay()
        relay.images = [state(0, NEW_HASH, DETAILS["version"])]
        device, report = Mock(), {}
        with patch.object(ota, "Relay", return_value=relay):
            ota.verify_with_fresh_session(device, io.StringIO(), DETAILS, report,
                                          progress=lambda value: None)
        device.close.assert_called_once()
        device.open.assert_called_once()
        self.assertEqual(relay.calls, [(0, 1, 0, {})])
        self.assertEqual(relay.reset_count, 0)
        self.assertTrue(report["verification_session_reopened"])
        self.assertEqual(report["result"], "installed_and_confirmed")

    def test_same_confirmed_image_has_no_upload_or_reset(self):
        relay = FakeRelay()
        relay.images = [state(0, NEW_HASH, DETAILS["version"])]
        self.assertEqual(self.run_upload(relay)["result"], "already_installed_and_confirmed")
        self.assertEqual(relay.modes, [])
        self.assertEqual(relay.reset_count, 0)
        self.assertEqual(len(relay.calls), 1)

    def test_existing_trial_or_pending_image_blocks_new_upload(self):
        for images in ([state(confirmed=False)], [state(), state(1, pending=True)]):
            relay = FakeRelay()
            relay.images = images
            with self.subTest(images=images), self.assertRaises(RuntimeError):
                self.run_upload(relay)
            self.assertEqual(relay.modes, [])
            self.assertEqual(relay.reset_count, 0)

    def test_out_of_range_or_stalled_upload_never_schedules_reset(self):
        for offset, error in ((-1, ValueError), (len(IMAGE) + 1, ValueError),
                              (True, ValueError), (0, RuntimeError)):
            relay = FakeRelay()
            relay.offset_override = offset
            with self.subTest(offset=offset), self.assertRaises(error):
                self.run_upload(relay)
            self.assertEqual(relay.reset_count, 0)
            self.assertFalse(any(call[:3] == (2, 1, 0) for call in relay.calls))

    def test_corrupted_secondary_identity_never_schedules_trial(self):
        original = ota.image_states
        calls = 0

        def corrupt_secondary(reply):
            nonlocal calls
            calls += 1
            result = original(reply)
            if calls == 2:
                result[1]["hash"] = OLD_HASH
            return result

        relay = FakeRelay()
        with patch.object(ota, "image_states", side_effect=corrupt_secondary), \
                self.assertRaises(RuntimeError):
            self.run_upload(relay)
        self.assertEqual(relay.reset_count, 0)
        self.assertFalse(any(call[:3] == (2, 1, 0) for call in relay.calls))


if __name__ == "__main__":
    unittest.main()
