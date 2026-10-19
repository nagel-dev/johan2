#!/usr/bin/env python3
"""Download /mic-aec.wav and split its simultaneous channels for auditioning."""
import argparse
from array import array
import io
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request
import wave

CHANNELS = ("mic-before-aec", "playback-reference", "mic-after-aec", "assist-input")


def split_wav(data: bytes, output: Path) -> None:
    with wave.open(io.BytesIO(data), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (4, 2, 16000):
            raise ValueError("Expected four-channel 16-bit 16 kHz AEC WAV")
        if source.getnframes() != 240000:
            raise ValueError("Expected a complete 15-second recording")
        pcm = source.readframes(source.getnframes())
        if len(pcm) != 240000 * 8:
            raise ValueError("Truncated WAV audio")
    samples = array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    output.mkdir(parents=True, exist_ok=True)
    (output / "mic-aec.wav").write_bytes(data)
    for channel, name in enumerate(CHANNELS):
        mono = samples[channel::4]
        if sys.byteorder != "little":
            mono.byteswap()
        with wave.open(str(output / f"{channel + 1}-{name}.wav"), "wb") as target:
            target.setnchannels(1)
            target.setsampwidth(2)
            target.setframerate(16000)
            target.writeframes(mono.tobytes())


def export_m4a(output: Path) -> None:
    converter = shutil.which("afconvert")
    if not converter:
        return
    # The macOS AAC encoder rejects this 16 kHz input/bitrate combination.
    # Resample a temporary mono copy to standard 44.1 kHz before AAC encoding.
    with tempfile.TemporaryDirectory(prefix="johan2-aac-") as temporary:
        for channel, name in enumerate(CHANNELS):
            source = output / f"{channel + 1}-{name}.wav"
            pcm = Path(temporary) / f"{channel}.wav"
            target = source.with_suffix(".m4a")
            converted = subprocess.run([converter, "-f", "WAVE", "-d", "LEI16@44100", str(source), str(pcm)],
                                       capture_output=True, text=True)
            if not converted.returncode:
                converted = subprocess.run([converter, "-f", "m4af", "-d", "aac", "-b", "96000", str(pcm), str(target)],
                                           capture_output=True, text=True)
            if converted.returncode:
                print(f"M4A conversion failed for {source.name}; mono WAV is available", file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", help="Device IP/hostname, or base URL such as http://192.168.1.50:8081")
    parser.add_argument("output", type=Path, help="New directory for this recording")
    parser.add_argument("--delay-ms", type=int, choices=range(151), metavar="0..150",
                        help="Change the diagnostic reference delay (persists until changed or reboot)")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output directory already exists; choose a new name to preserve previous recordings")
    base = args.device if "://" in args.device else f"http://{args.device}:8081"
    url = base.rstrip("/") + "/mic-aec.wav?channel=all"
    if args.delay_ms is not None:
        url += f"&delay_ms={args.delay_ms}"
    print("Recording 15 seconds for AEC comparison.", flush=True)
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read(2000000)
            metrics = response.headers.get("X-Mic-Metrics", "No metrics returned")
            channels = response.headers.get("X-Mic-Channels", "")
        split_wav(data, args.output)
    except urllib.error.HTTPError as error:
        parser.exit(1, f"Device returned HTTP {error.code}: {error.read(1024).decode(errors='replace')}\n")
    except (OSError, ValueError, wave.Error) as error:
        parser.exit(1, f"Capture failed: {error}\n")
    (args.output / "metrics.txt").write_text(metrics + "\n" + channels + "\n")
    # Music cannot reliably import the four-channel analysis WAV. On macOS also
    # produce individual AAC/M4A tracks using its built-in audio converter.
    export_m4a(args.output)
    print(f"Saved comparison WAV, four mono tracks, and metrics to {args.output}")
    print(metrics)
    fields = dict(item.strip().split("=", 1) for item in metrics.split(";") if "=" in item)
    if fields.get("aec_frames") == "0":
        print("No frames were processed by AEC; this recording cannot establish echo suppression.", file=sys.stderr)


if __name__ == "__main__":
    main()
