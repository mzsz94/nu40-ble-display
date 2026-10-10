"""Check remote KIT completion evidence instead of treating queue acceptance as success."""

import copy
import unittest
from unittest.mock import patch

import kit_control as kit


def status(**changes):
    value = dict.fromkeys(kit.UNSIGNED | kit.RESULTS, 0)
    value.update(schema=1, keys=15, version="0.1.2", file="", **changes)
    return value


class FakeRelay:
    def __init__(self, states, reply=None):
        self.states = iter(states)
        self.reply = reply
        self.calls = []

    def request(self, op, group, command, values):
        self.calls.append((op, group, command, copy.deepcopy(values)))
        if op == 0:
            return next(self.states)
        return self.reply if self.reply is not None else {
            "queued": values["action"], "value": values["value"]}


class KitControlTests(unittest.TestCase):
    def test_transient_relay_timeout_retries_only_read_requests(self):
        from unittest.mock import Mock

        relay = Mock()
        relay.request.side_effect = [RuntimeError("NU87 SMP relay failed: -2"), status()]
        with patch.object(kit.time, "sleep"):
            kit.read_status(relay)
        self.assertEqual(relay.request.call_count, 2)
        for call in relay.request.call_args_list:
            self.assertEqual(call.args, (0, 64, 0, {}))
        relay.request.reset_mock()
        relay.request.side_effect = RuntimeError("Remote SMP error: {'rc': 3}")
        with self.assertRaises(RuntimeError):
            kit.read_status(relay)
        self.assertEqual(relay.request.call_count, 1)

    def test_status_bounds_types_and_required_fields(self):
        original = status()
        self.assertEqual(kit.check_status(original), original)
        for key, value in (("schema", True), ("peak", 32769), ("samples", 32001),
                           ("busy", 5), ("page", 3), ("keys", 16), ("sd_mounted", 2),
                           ("rms", -1), ("plays", True), ("sd_result", "ok"),
                           ("file", "a" * 40), ("version", None)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                kit.check_status({**original, key: value})
        for key in kit.UNSIGNED | kit.RESULTS:
            damaged = original.copy()
            del damaged[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                kit.check_status(damaged)

    def test_queue_acceptance_waits_for_completed_capture_and_counter(self):
        relay = FakeRelay([status(), status(busy=2, samples=16000),
                           status(records=1, samples=32000)])
        with patch.object(kit.time, "sleep"):
            before, after = kit.perform(relay, "record")
        self.assertEqual(before["records"], 0)
        self.assertEqual(after["records"], 1)
        self.assertEqual(relay.calls[1], (2, 64, 1, {"action": 2, "value": 0}))

    def test_partial_capture_or_missing_completion_is_not_success(self):
        for after in (status(records=1, samples=100), status(samples=32000),
                      status(audio_result=-5)):
            with self.subTest(after=after), self.assertRaises(RuntimeError):
                kit.perform(FakeRelay([status(), after]), "record")

    def test_ram_capture_can_succeed_without_sd_filesystem(self):
        _, after = kit.perform(FakeRelay([status(), status(records=1, samples=32000,
                                                          sd_result=-19)]), "record")
        self.assertEqual(after["sd_saves"], 0)
        self.assertEqual(after["sd_result"], -19)

    def test_play_failure_and_mismatched_action_are_rejected(self):
        with self.assertRaises(RuntimeError):
            kit.perform(FakeRelay([status(), status(audio_result=-61)]), "play")
        with self.assertRaises(ValueError):
            kit.perform(FakeRelay([status()], reply={"queued": 2, "value": 0}), "beep")

    def test_page_and_rgb_are_verified_from_applied_state(self):
        for action, key in (("page", "page"), ("rgb", "rgb_mode")):
            with self.subTest(action=action):
                kit.perform(FakeRelay([status(), status(**{key: 2})]), action, 2)
                with self.assertRaises(RuntimeError):
                    kit.perform(FakeRelay([status(), status()]), action, 2)

    def test_invalid_actions_do_not_send_any_request(self):
        relay = FakeRelay([])
        for action, value in (("erase", 0), ("record", 1), ("page", 3), ("rgb", True)):
            with self.subTest(action=action), self.assertRaises(ValueError):
                kit.perform(relay, action, value)
        self.assertEqual(relay.calls, [])

    def test_wrapped_counter_is_a_valid_single_completion(self):
        kit.perform(FakeRelay([status(beeps=0xffffffff), status(beeps=0)]), "beep")


if __name__ == "__main__":
    unittest.main()
