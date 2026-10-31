"""Exercise the HA package's real templates and script branches with fake services."""
import json
import math
from datetime import datetime, timezone
from pathlib import Path

import yaml
from jinja2.nativetypes import NativeEnvironment


class Loader(yaml.SafeLoader):
    pass


Loader.add_constructor("!secret", lambda loader, node: loader.construct_scalar(node))
root = Path(__file__).resolve().parents[1]
package = yaml.load((root / "ha/packages/johan2_bose.yaml").read_text(), Loader=Loader)


def from_json(value, default=None):
    try:
        return json.loads(value)
    except (ValueError, TypeError):
        return default


env = NativeEnvironment()
env.filters.update(
    from_json=from_json,
    to_json=json.dumps,
    bool=lambda value: value if isinstance(value, bool) else str(value).lower() in ("true", "1", "on"),
)
env.globals["now"] = lambda: datetime(2026, 10, 31, tzinfo=timezone.utc)


def render(value, context):
    if isinstance(value, str) and ("{{" in value or "{%" in value):
        return env.from_string(value).render(context)
    return value


def run(calibrate=False, reference=None, response=None):
    entities = {
        "input_text.johan2_bose_reference": json.dumps(reference) if reference is not None else "unknown",
        "input_text.johan2_bose_result": '{"status":"goed","percent":80}',
    }
    context = {"calibrate": calibrate, "states": lambda entity: entities[entity]}
    calls = []

    def execute(sequence):
        for step in sequence:
            if "variables" in step:
                for key, value in step["variables"].items():
                    context[key] = render(value, context)
            elif "if" in step:
                condition = render(step["if"], context)
                assert isinstance(condition, bool), condition
                execute(step["then"] if condition else step.get("else", []))
            elif step.get("action") == "input_text.set_value":
                value = render(step["data"]["value"], context)
                entities[step["target"]["entity_id"]] = value if isinstance(value, str) else json.dumps(value)
                assert len(entities[step["target"]["entity_id"]]) <= 255
            elif step.get("action") == "rest_command.johan2_bose_measure":
                calls.append("measure")
                if response is not None:
                    context[step["response_variable"]] = response
            else:
                raise AssertionError(step)

    execute(package["script"]["johan2_bose_check"]["sequence"])
    return from_json(entities["input_text.johan2_bose_result"]), from_json(entities["input_text.johan2_bose_reference"]), calls


def response(percent=100, valid=(True, True, True)):
    delta = 20 * math.log10(percent / 100)
    return {
        "status": 200,
        "content": {
            "valid": True,
            "bands": [
                {"hz": hz, "gain_db": gain + delta, "valid": is_valid}
                for hz, gain, is_valid in zip((93.75, 156.25, 218.75), (-25, -30, -35), valid)
            ],
        },
    }


result, reference, calls = run(calibrate=True, response=response(valid=(True, False, True)))
assert result["status"] == "referentie opgeslagen" and calls == ["measure"]
assert reference == {"version": 1, "bands": [-25.0, None, -35.0]}
for percent, status in ((44.9, "te laag"), (45, "goed"), (50, "goed"), (55, "goed"), (55.1, "te hoog"), (80, "te hoog")):
    result, _, calls = run(reference=reference, response=response(percent))
    assert result["status"] == status and result["percent"] == percent and calls == ["measure"], result
result, _, calls = run(reference=reference, response=response(valid=(True, True, False)))
assert result["status"] == "onvoldoende overeenkomstige tonen" and "percent" not in result
result, _, calls = run()
assert result["status"] == "geen referentie" and not calls
for bad_reference in ([], {"version": 1, "bands": None}, {"version": 1, "bands": "bad"}):
    result, _, calls = run(reference=bad_reference, response=response())
    assert result["status"] == "geen referentie" and not calls
for failed in (None, {"status": 409, "content": {"valid": False}}, {"status": 200, "content": {"valid": False}}):
    result, saved, calls = run(calibrate=True, reference=reference, response=failed)
    assert result["status"] == ("bezet" if failed and failed["status"] == 409 else "meting mislukt"), (result, failed)
    assert saved == reference and "percent" not in result and calls == ["measure"]

status_template = package["template"][0]["sensor"][0]["state"]
for saved, expected in (
    ({"status": "goed", "percent": 80}, "te hoog"),
    ({"status": "te laag", "percent": 50}, "goed"),
    ({"status": "te laag", "percent": 44.9}, "te laag"),
    ({"status": "meting mislukt"}, "meting mislukt"),
):
    status = render(status_template, {"states": lambda _: json.dumps(saved)}).strip()
    assert status == expected, (saved, status)

print("HA Bose package: calibration, boundaries, failures and mismatched bands passed")
