"""Exercise the family calendar package with synthetic events and HA-like templates."""
import ast
from datetime import datetime, time, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import yaml
from jinja2 import Environment, StrictUndefined

root = Path(__file__).resolve().parents[1]
script = yaml.safe_load((root / "ha/packages/johan2_family_calendar.yaml").read_text())["script"]["johan2_family_calendar_search"]
tz = ZoneInfo("Europe/Amsterdam")
clock = datetime(2026, 10, 31, 15, 0, tzinfo=tz)
env = Environment(undefined=StrictUndefined)


def strptime(value, fmt, default=None):
    try:
        return datetime.strptime(value, fmt)
    except ValueError:
        return default


env.globals.update(
    strptime=strptime,
    timedelta=timedelta,
    now=lambda: clock,
    today_at=lambda value: datetime.combine(clock.date(), time.fromisoformat(value), tzinfo=tz),
)


def render(value, context):
    if not isinstance(value, str) or ("{{" not in value and "{%" not in value):
        return value
    result = env.from_string(value).render(context).strip()
    try:
        parsed = ast.literal_eval(result)
    except (ValueError, SyntaxError):
        return result
    return result if isinstance(parsed, (str, complex)) else parsed


def run(fields=None, events=None, response=None):
    context = dict(fields or {})
    calls = []

    def execute(sequence):
        for step in sequence:
            if "variables" in step:
                for key, value in step["variables"].items():
                    context[key] = render(value, context)
            elif "if" in step:
                if render(step["if"], context):
                    result = execute(step["then"])
                    if result is not None:
                        return result
            elif step.get("action") == "calendar.get_events":
                assert step["target"]["entity_id"] == "calendar.family"
                calls.append({key: render(value, context) for key, value in step["data"].items()})
                context[step["response_variable"]] = response if response is not None else {
                    "calendar.family": {"events": events if events is not None else []}
                }
            elif "stop" in step:
                if step.get("error"):
                    raise ValueError(step["stop"])
                return context[step["response_variable"]]
            else:
                raise AssertionError(step)
        return None

    return execute(script["sequence"]), calls


events = [
    {"summary": "Voetbal", "start": "2026-11-02T10:00:00+01:00",
     "end": "2026-11-02T11:00:00+01:00", "description": "privé", "location": "privé"},
    {"summary": "Verjaardag", "start": "2026-11-03", "end": "2026-11-04"},
]
result, calls = run({"query": "VOET"}, events)
assert result["start_date"] == "2026-10-31" and result["end_date_inclusive"] == "2027-10-30"
assert result["events"] == [{"title": "Voetbal", "start": events[0]["start"], "end": events[0]["end"]}]
assert calls[0]["start_date_time"] == "2026-10-31 00:00:00+01:00"
assert calls[0]["end_date_time"] == "2027-10-31 00:00:00+02:00"
result, calls = run({"start_date": "2027-03-28", "end_date": "2027-03-28"}, events)
assert result["end_date_inclusive"] == "2027-03-28" and len(result["events"]) == 2
assert calls[0]["start_date_time"] == "2027-03-28 00:00:00+01:00"
assert calls[0]["end_date_time"] == "2027-03-29 00:00:00+02:00"
assert run({"query": "onbekend"}, events)[0]["events"] == []
clock = datetime(2028, 2, 29, 12, 0, tzinfo=tz)
assert run()[0]["end_date_inclusive"] == "2029-02-27"
for fields in (
    {"start_date": "ongeldig"}, {"start_date": "2026-2-03"},
    {"end_date": "2026-02-30"}, {"start_date": "2027-01-02", "end_date": "2027-01-01"},
):
    result, calls = run(fields)
    assert "error" in result and not calls
result, _ = run(response={"calendar.family": {}})
assert "error" in result

print("Family calendar search tests passed")
