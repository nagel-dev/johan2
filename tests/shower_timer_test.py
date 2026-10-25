"""Exercise the actual HA templates and sentence grammar without speakers/network.

Requires PyYAML, Jinja2 and hassil. This is not a Home Assistant integration test.
"""
from datetime import datetime, timezone
import ast
import json
from pathlib import Path
import unittest

from hassil import Intents, recognize
from jinja2 import StrictUndefined
from jinja2.nativetypes import NativeEnvironment
import yaml


PACKAGE = yaml.safe_load((Path(__file__).parents[1] / "ha/shower_timer.yaml").read_text())
VOICE, REMINDERS = PACKAGE["automation"]


def from_json(value, default=None):
    try:
        return json.loads(value)
    except (ValueError, TypeError):
        return default


def timestamp(value, default=0):
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, datetime):
        return value.timestamp()
    try:
        return datetime.fromisoformat(value).timestamp()
    except (ValueError, TypeError):
        return default


class Harness:
    def __init__(self):
        self.clock = 1000000
        self.state = "idle"
        self.attrs = {}
        self.saved = ""
        self.env = NativeEnvironment(undefined=StrictUndefined)
        self.env.filters.update(from_json=from_json, to_json=json.dumps)
        self.env.globals.update(
            states=lambda entity: self.state if entity == "timer.douchetimer" else self.saved,
            state_attr=lambda entity, key: self.attrs.get(key),
            is_state=lambda entity, value: (self.state if entity == "timer.douchetimer" else self.saved) == value,
            has_value=lambda entity: self.state not in ("unknown", "unavailable")
            if entity == "timer.douchetimer" else self.saved not in ("unknown", "unavailable"),
            now=lambda: datetime.fromtimestamp(self.clock, timezone.utc),
            as_timestamp=timestamp,
        )

    def render(self, template, **variables):
        result = self.env.from_string(template).render(**variables)
        # HA strips template whitespace before parsing native values.
        if isinstance(result, str):
            result = result.strip()
            try:
                return ast.literal_eval(result)
            except (ValueError, SyntaxError):
                pass
        return result

    def tick(self):
        if not self.render(REMINDERS["conditions"][0]["value_template"]):
            return ""
        variables = {}
        for action in REMINDERS["actions"][:2]:
            for key, value in action["variables"].items():
                variables[key] = self.render(value, **variables)
        result = variables["outcome"]
        if result["checkpoint"] != variables["checkpoint"]:
            self.saved = json.dumps(result["checkpoint"])
        return result["message"]

    def start(self, seconds):
        self.state = "active"
        self.attrs = {"finishes_at": self.clock + seconds, "last_transition": "started"}
        return self.tick()

    def finish(self, transition="finished"):
        self.state = "idle"
        self.attrs = {"last_transition": transition}
        return self.tick()


class ShowerTimerTests(unittest.TestCase):
    def setUp(self):
        self.h = Harness()

    def test_ten_minutes_announces_each_threshold_and_finish_once(self):
        self.assertEqual(self.h.start(600), "")
        for elapsed in range(1, 601):
            self.h.clock += 1
            message = self.h.tick() if elapsed < 600 else self.h.finish()
            expected = {300: "Je hebt nog vijf minuten douchetijd.",
                        540: "Nog één minuut douchetijd.",
                        600: "Je douchetijd is voorbij."}.get(elapsed, "")
            self.assertEqual(message, expected, elapsed)
        self.assertEqual(self.h.tick(), "")

    def test_short_timer_skips_past_thresholds(self):
        self.assertEqual(self.h.start(120), "")
        self.h.clock += 60
        self.assertEqual(self.h.tick(), "Nog één minuut douchetijd.")
        self.assertEqual(self.h.tick(), "")
        self.assertEqual(self.h.start(30), "")
        self.h.clock += 30
        self.assertEqual(self.h.finish(), "Je douchetijd is voorbij.")

    def test_cancel_never_produces_finished_message(self):
        self.h.start(600)
        self.assertEqual(self.h.finish("cancelled"), "")
        self.h.clock += 1000
        self.assertEqual(self.h.tick(), "")

    def test_pause_resume_and_duration_change(self):
        self.h.start(600)
        self.h.clock += 300
        self.assertIn("vijf", self.h.tick())
        self.h.state = "paused"
        self.h.attrs = {"remaining": "00:05:00", "last_transition": "paused"}
        self.assertEqual(self.h.tick(), "")
        self.h.clock += 1000
        self.assertEqual(self.h.tick(), "")
        self.assertEqual(self.h.start(300), "")
        self.h.clock += 240
        self.assertIn("één", self.h.tick())
        # A fresh, longer deadline creates a fresh reminder schedule.
        self.assertEqual(self.h.start(420), "")
        self.h.clock += 120
        self.assertIn("vijf", self.h.tick())
        # Shortening beyond a threshold skips that old reminder.
        self.assertEqual(self.h.start(40), "")
        self.assertEqual(self.h.tick(), "")

    def test_restart_preserves_flags_and_announces_missed_expiry_once(self):
        self.h.start(600)
        self.h.clock += 300
        self.assertIn("vijf", self.h.tick())
        restored = Harness()
        restored.saved, restored.state, restored.attrs = self.h.saved, self.h.state, self.h.attrs.copy()
        restored.clock = self.h.clock + 20
        self.assertEqual(restored.tick(), "")
        restored.clock += 300
        self.assertEqual(restored.finish(), "Je douchetijd is voorbij.")
        # The completed checkpoint survives another restart without replaying.
        again = Harness()
        again.saved = restored.saved
        again.attrs = restored.attrs.copy()
        self.assertEqual(again.tick(), "")

    def test_unavailable_does_not_destroy_checkpoint_or_replay_history(self):
        # A newly added input_text without an initial value starts as unknown.
        self.h.saved = "unknown"
        self.h.start(600)
        saved = self.h.saved
        self.h.state = "unavailable"
        self.assertEqual(self.h.tick(), "")
        self.assertEqual(self.h.saved, saved)
        # First installation must not replay a pre-existing finished timer.
        self.h.saved = ""
        self.assertEqual(self.h.finish(), "")

    def test_delayed_poll_uses_only_latest_reminder(self):
        self.h.start(600)
        self.h.clock += 550
        self.assertEqual(self.h.tick(), "Nog één minuut douchetijd.")
        self.assertEqual(self.h.tick(), "")

    def test_duration_validation_and_default(self):
        template = VOICE["actions"][0]["choose"][1]["sequence"][0]["variables"]["requested_minutes"]
        for word, expected in [("8", 8), ("acht", 8), ("één", 1), ("tweeëntwintig", 22),
                               ("negenenvijftig", 59), ("zestig", 60), ("1.5", 0),
                               ("niets", 0), ("-2", -2)]:
            actual = self.h.render(template, trigger={"slots": {"minutes": word}})
            self.assertEqual(actual, expected, word)
        self.assertEqual(self.h.render(template, trigger={"slots": {}}), 10)
        invalid_guard = PACKAGE["script"]["shower_timer_start"]["sequence"][0]["if"]
        self.assertFalse(self.h.render(invalid_guard))
        for invalid in (0, -1, 61, 1.5, "wrong"):
            self.assertTrue(self.h.render(invalid_guard, minutes=invalid), invalid)

    def test_spoken_grammar_and_slots(self):
        grammar = Intents.from_dict({
            "language": "nl",
            "intents": {t["id"]: {"data": [{"sentences": t["command"]}]}
                        for t in VOICE["triggers"]},
            "lists": {"minutes": {"wildcard": True}},
        })
        for sentence, intent in [
            ("start de douchetimer", "start"), ("ik ga douchen", "start"),
            ("zet de douchetimer op 8 minuten", "duration"),
            ("stel de douche timer in op acht minuten", "duration"),
            ("stop de douchetimer", "cancel"),
            ("hoeveel tijd heeft de douchetimer nog", "status"),
            ("hoe lang heb ik nog om te douchen", "status"),
        ]:
            result = recognize(sentence, grammar)
            self.assertIsNotNone(result, sentence)
            self.assertEqual(result.intent.name, intent)
        self.assertIsNone(recognize("start een kooktimer", grammar))
        self.assertIsNone(recognize("hoe lang heb ik nog", grammar))

    def test_status_uses_deadline_and_never_reports_negative_time(self):
        response = PACKAGE["intent_script"]["Johan2ShowerTimerStatus"]["speech"]["text"]
        self.h.start(600)
        self.h.clock += 123
        self.assertIn("7 minuten en 57 seconden", self.h.render(response))
        self.h.clock += 1000
        self.assertIn("0 minuten en 0 seconden", self.h.render(response))


if __name__ == "__main__":
    unittest.main()
