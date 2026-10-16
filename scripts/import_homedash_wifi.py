#!/usr/bin/env python3
"""Copy only HomeDash Wi-Fi settings into ignored local build defaults."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path, help="Path to HomeDash sdkconfig")
args = parser.parse_args()
values = {}
for line in args.source.read_text().splitlines():
    key, separator, value = line.partition("=")
    if separator and key in ("CONFIG_HOMEDASH_WIFI_SSID", "CONFIG_HOMEDASH_WIFI_PASSWORD"):
        values[key] = json.loads(value)
ssid = values.get("CONFIG_HOMEDASH_WIFI_SSID", "")
password = values.get("CONFIG_HOMEDASH_WIFI_PASSWORD", "")
if not ssid or ssid == "your_ssid" or password == "your_password":
    parser.error("HomeDash Wi-Fi settings are missing or still placeholders")
output = Path(__file__).resolve().parents[1] / "sdkconfig.local"
output.write_text(
    "# Private Wi-Fi defaults imported from HomeDash; do not commit.\n"
    + "CONFIG_JOHAN2_WIFI_SSID=" + json.dumps(ssid) + "\n"
    + "CONFIG_JOHAN2_WIFI_PASSWORD=" + json.dumps(password) + "\n"
)
output.chmod(0o600)
print("Imported Wi-Fi settings into sdkconfig.local (values hidden).")
