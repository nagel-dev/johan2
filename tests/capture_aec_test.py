"""Verify channel order, signed PCM preservation, and rejection of bad captures."""
from array import array
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import wave

spec = importlib.util.spec_from_file_location("capture_aec", Path(__file__).parents[1] / "scripts/capture_aec.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def wav(channels=4, frames=240000):
    buffer = io.BytesIO()
    samples = array("h", [-32768, -123, 456, 32767][:channels] * frames)
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(buffer, "wb") as target:
        target.setnchannels(channels)
        target.setsampwidth(2)
        target.setframerate(16000)
        target.writeframes(samples.tobytes())
    return buffer.getvalue()


with tempfile.TemporaryDirectory() as temp:
    output = Path(temp)
    data = wav()
    module.split_wav(data, output)
    assert (output / "mic-aec.wav").read_bytes() == data
    for i, expected in enumerate([-32768, -123, 456, 32767]):
        with wave.open(str(output / f"{i+1}-{module.CHANNELS[i]}.wav")) as track:
            assert (track.getnchannels(), track.getframerate(), track.getnframes()) == (1, 16000, 240000)
            assert int.from_bytes(track.readframes(1), "little", signed=True) == expected
    for invalid in (wav(1), wav(frames=100), data[:-100]):
        try:
            module.split_wav(invalid, output)
        except ValueError:
            pass
        else:
            raise AssertionError("Accepted an invalid capture")
print("AEC capture utility tests passed")
