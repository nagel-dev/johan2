# Johan2 AUX player

ESP32-S3 N16R8 firmware that receives Sendspin audio from Music Assistant and
outputs stereo line-level audio through a PCM5102A DAC. Spotify stays configured
in Music Assistant; this device needs only Wi-Fi. No separate server or Home
Assistant access token is needed.

## Hardware

| ESP32-S3 | PCM5102A |
| --- | --- |
| GPIO6 | BCK / BCLK |
| GPIO5 | LCK / LRCK / WS |
| GPIO4 | DIN |
| GND | GND |

Keep the DAC power wiring and any module solder bridges from your working
Arduino setup. Module supply requirements vary; do not change them based on
bare-chip voltage ratings. Plug the DAC's 3.5 mm output into an amplifier or
powered speaker AUX input. This is a line output, not a passive-speaker driver.

Target: ESP32-S3, 16 MB flash, 8 MB octal PSRAM. No display or buttons required.

## Build and flash

Use ESP-IDF **6.0.1** with the ESP32-S3 toolchain installed. The helper uses
`$IDF_PATH`, or `~/esp/v6.0.1/esp-idf` by default, and fixes the missing Xtensa
PATH entry in installations previously used only for HomeDash's ESP32-C6.

```bash
./scripts/idf.sh menuconfig
# Johan2 Player Configuration: Wi-Fi SSID/password, name, optional server URL
./scripts/idf.sh build
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

Choose the actual serial port. Exit the monitor with Ctrl+]. For boards with
separate USB/UART sockets, use the UART socket for the default serial console.
If download mode is needed, hold BOOT, tap RESET, then release BOOT.
Flashing replaces the existing Arduino sketch.

The generated `sdkconfig`, `sdkconfig.local`, build output, and downloaded
components are ignored by Git. Wi-Fi credentials are compiled into your local
firmware and stored in NVS at boot; keep that firmware and local build
configuration private. Nonempty menuconfig Wi-Fi settings replace stored
credentials; empty settings reuse credentials already in NVS.

### Optional HomeDash Wi-Fi import

HomeDash supplied the configuration pattern, ESP-IDF version, logging style,
and Wi-Fi power-save choice. Its C6/display/HA settings are not suitable for
this S3 audio board and are not copied. Its current saved Wi-Fi fields are
placeholders, so enter real credentials in menuconfig.

If HomeDash later has real credentials, before the first configuration run:

```bash
python3 scripts/import_homedash_wifi.py ../homedash/sdkconfig
```

This copies **only** Wi-Fi settings into ignored `sdkconfig.local`; values are
not printed. Defaults seed a new sdkconfig only. If already configured, update
Wi-Fi with menuconfig instead. HomeDash files are never modified.

## Music Assistant

1. Connect the ESP32 to a Wi-Fi network that can communicate with Music Assistant.
   Guest networks often isolate clients or block multicast discovery.
2. Ensure Music Assistant's Sendspin player provider is enabled.
3. Flash and watch the serial monitor for an IP address,
   `Firmware 0.2.2; Sendspin v0.8.0 legacy transport`, and `Player ready`.
4. Look for **Johan2 AUX** in Music Assistant's players. Select it and play music
   from your existing Spotify integration. If Music Assistant requests approval,
   allow legacy/unencrypted clients in the Sendspin provider settings. Start with the amplifier volume low.

The player advertises `_sendspin._tcp`, port **8928**, path `/sendspin`.
Allow mDNS (UDP 5353) and TCP 8928 between Music Assistant and the player.
If discovery is unavailable but direct connectivity works, set the optional
WebSocket URL to `ws://MUSIC_ASSISTANT_IP:8928/sendspin`. This is the Sendspin
port, not the Music Assistant web interface port.

The firmware uses official [sendspin-cpp v0.8.0](https://github.com/Sendspin/sendspin-cpp/releases/tag/v0.8.0),
pinned to commit `9331ace6428979982c934384702d289958ca125e`, plus a committed
component lockfile. It uses the legacy unencrypted protocol for stable Music
Assistant 2.10.x, including 2.10.6. Enable legacy/unencrypted client support in
the Music Assistant Sendspin provider if disabled. There is no pairing or
transport authentication in this firmware version; use your trusted internal LAN.

The previous v0.9.5 build required a newer Noise handshake (`psk_category`)
that MA 2.10.x does not send. Sharing Noise support alone did not establish
compatibility; the observed handshake failure supersedes the earlier source
assessment.

The player ID is derived from the board MAC and survives reboots. Last-server
preference and static output delay are stored in NVS. Existing Wi-Fi and old
pairing blobs are preserved; the legacy client does not use the old pairing
records. The MAC-derived ID differs from the old key-derived ID, so Music
Assistant may show a new player entry. No flash erase is needed.

## Implementation and limits

- `main/network.cpp`: automatic Wi-Fi retry with backoff, without blocking the
  ESP event loop; modem sleep disabled for audio.
- `main/audio_output.cpp`: Philips I2S, stereo 48 kHz / 16-bit, digital volume
  and mute, cleared DMA buffers between streams. The hardware queue is 30 ms;
  the original 2048-byte write buffer and 100 Hz RTOS tick are restored after
  the smaller queue caused bad audio. DMA completion timestamps feed
  Sendspin synchronization from task context, never directly from the ISR.
- `main/main.cpp`: Sendspin player role, mDNS advertisement, NVS persistence,
  and optional outgoing server connection.
- Initial volume is 35%; Music Assistant can change it. It is independent of
  the Arduino library's 0–21 scale.
- Only FLAC and PCM at 48 kHz, stereo, 16-bit are advertised. Music Assistant
  handles source conversion. No direct Spotify login or Spotify Connect.
- The partition table reserves a 6 MB factory app. USB flashing only; OTA is
  not implemented.

The firmware is built with ESP-IDF 6.0.1.
Build verification and hardware verification are separate. Playback,
Wi-Fi recovery, and synchronization must still be checked on the physical board
with your Music Assistant version. For multi-room playback, compare with another
player and adjust output delay if the connected amplifier adds latency.

## Home Assistant-aankondigingen

Zie [ha/README.md](ha/README.md) voor het handmatig testen en de blueprint
voor een gesproken melding wanneer de achterdeur opengaat.

### INMP441 microphone LED test

Wire VDD to 3V3 (never 5V), GND and L/R to GND, SCK to GPIO15,
WS to GPIO16, and SD to GPIO17. The microphone uses a separate I2S controller
at 16 kHz with 32-bit left-channel slots. The RGB LED retains its coloured music effect. Sound above the microphone noise
threshold temporarily overrides it with white; brightness follows RMS level with
a short decay, capped at 35% per colour. Keep quiet for the first second after
startup while ambient noise is calibrated. In silence it returns to music colours
or turns off when no music is playing.
Without an HA token the microphone is measured locally only. Enabling Assist
streams microphone PCM over the LAN to Home Assistant for wake-word detection;
HA Cloud processes command audio after the wake word. No recordings are stored
by this firmware.
Physical microphone/LED operation still needs verification after flashing.

### Hey Jarvis via Home Assistant Assist

1. Install/start the openWakeWord app on HA OS and add its discovered Wyoming
   integration. Edit the preferred voice assistant, use the three-dot menu to add
   a streaming wake word, and choose **openWakeWord → hey jarvis**. Keep your
   working HA Cloud speech-to-text and conversation agent settings. The firmware
   uses the preferred pipeline; `JOHAN2_HA_PIPELINE` can select another pipeline ID.
2. Create a long-lived access token in your HA profile under Security. Run
   `./scripts/idf.sh menuconfig`, open **Johan2 Player Configuration**, and set
   **Home Assistant long-lived access token**. Leave the WebSocket URL at
   `ws://haos.home:8123/api/websocket`. Save to the default `sdkconfig` file,
   which is ignored by Git. Do not paste the token into chat or tracked files.
   The optional pipeline ID stays blank when using your preferred assistant.
   To hear replies, set **HA media_player entity for spoken replies** to the HA
   entity ID of this player's Music Assistant entity (`media_player.…`) and select
   a text-to-speech provider (such as Piper) in your HA assistant.
3. Run `./scripts/idf.sh build`, then flash using your actual serial port.
4. Say **“Hey Jarvis, volgend nummer op Johan2 AUX”**. The explicit player name
   matters: this custom WebSocket client has no HA device/area registration.

The default endpoint is `ws://haos.home:8123/api/websocket`
(`JOHAN2_HA_WS_URL`). This is plain LAN WebSocket traffic, including the token.
Microphone audio is 16 kHz / mono / signed 16-bit little-endian PCM. The mic task
never waits for networking: a separate bounded queue and task handle authentication,
streaming, reconnects, pipeline errors, and automatic rearming. The capture stream
is independent of the LED noise gate. `JOHAN2_MIC_GAIN` defaults to 16 and can be
adjusted if speech clips or is too quiet.

Music retains the coloured LED effect, with white microphone activity as before.
After wake-word detection or remote command activation, music is digitally muted
while the command is active, including capture and processing. It is restored
before the spoken reply, cancellation or completion without changing the player
volume setting. During direct spoken replies, music remains at one fifth volume. Acoustic echo cancellation is optional and disabled by default; without it, loud
speaker audio can still impair wake-word detection. See the AEC test below. When `JOHAN2_HA_REPLY_PLAYER` is set, the pipeline
continues through text-to-speech. Its generated audio is forwarded to that HA
media player using `media_player.play_media` with `announce: true`; Music Assistant
handles playback and restoration of the music queue through Sendspin. Firmware
ducking ends before the reply so speech is not attenuated. Capture stays stopped
while the announcement service request is pending, with a 90-second watchdog and
no automatic announcement retry (to avoid duplicate replies). Without a reply
player configured, commands still work but there are no spoken answers.

The selected HA conversation agent must be allowed to see relevant entities. For
“is de achterdeur open?”, expose the door sensor to Assist and give it a clear
name/alias. Audio routing cannot supply entity state the agent cannot access.

Logs report connection/authentication, wake detection and generic errors, without
tokens, recognized text or server payloads. A rejected token disables Assist until
reboot. Missing token leaves the player and microphone LED test functional.
Firmware compilation and host protocol tests do not verify physical wake-word
recognition, LAN connectivity or playback quality.

Host protocol check (requires the managed ArduinoJson component):

```sh
c++ -std=c++17 -Wall -Wextra -Werror -I main \
  -I managed_components/bblanchon__arduinojson/src \
  tests/assist_protocol_test.cpp -o /tmp/johan2-assist-protocol-test
/tmp/johan2-assist-protocol-test
```

Protocol reference: <https://developers.home-assistant.io/docs/voice/pipelines/>.

### Temporary microphone recording diagnostics

Enable `JOHAN2_MIC_DIAGNOSTICS` in `./scripts/idf.sh menuconfig`, build and flash.
It is disabled by default; the current local sdkconfig enables it for investigation.
While enabled, the device exposes an unauthenticated HTTP endpoint on port 8081;
use it temporarily on a trusted LAN and disable it after testing.

Start a fresh 15-second recording (replace DEVICE_IP with the device address):

```sh
curl --fail --max-time 30 -D mic-headers.txt -o mic.wav http://DEVICE_IP:8081/mic.wav
```

The request starts capture immediately. Keep music off, leave about three seconds
of silence, speak a sentence nearby, pause, and repeat from your usual position.
Do not say the wake word during this baseline test: Assist remains active and
could otherwise change music volume or play a reply during capture.
The WAV contains the same post-DC-removal, post-gain, clipped 16 kHz mono PCM
submitted to Assist, including periods when Assist is not accepting audio. It is
before Home Assistant processing and independent of the LED noise threshold.
1,920,000 bytes are reserved in PSRAM on first use and reused. No audio is written
to flash; completed recordings are erased after the download attempt.

The `X-Mic-Metrics` response header reports whole-recording RMS and peak in dBFS,
the number of samples at either clipping rail, I2S read errors/timeouts, elapsed
capture time, maximum interval between captured blocks, and configured gain.
Clipped samples indicate saturated output, not necessarily microphone hardware
clipping. RMS includes both silence and speech; inspect these sections separately
when assessing background noise. Block intervals and read errors are clues to
interruptions, not proof that the I2S DMA stream lost no samples. This diagnostic
does not measure downstream Assist queue/network losses. A stalled capture returns
an HTTP error rather than presenting incomplete audio as a complete recording.

Build success does not verify microphone wiring, signal quality, or playback.
The initial hardware smoke test downloaded a valid 15-second WAV with no reported
read errors and 0.195% rail-clipped samples at gain 16; acoustic conditions were
uncontrolled, so a deliberate silence/near/far speech test is still required.

### Playback-reference AEC and music-on testing

Enable `JOHAN2_MIC_AEC` and `JOHAN2_MIC_DIAGNOSTICS` in menuconfig, then build
and flash via `./scripts/idf.sh`. AEC is experimental and off by default. The local
ignored sdkconfig enables it with fixed mic gain 4 and automatic gain disabled.
The flashed device passed boot and endpoint captures. Speaker-bleed attenuation
has been measured as described below; music-on wake-word accuracy remains unverified. No new microphone wiring is needed. ESP-SR is managed and pinned to
2.4.2; AEC uses its speech-recognition mode without loading a wake-word model.

The reference comes from the PCM actually sent by I2S, after volume, mute, music
and direct-TTS mixing. Interrupt callbacks copy fixed-size DMA blocks into bounded
queues; resampling and AEC run only in the capture task. A 63-tap low-pass converts
the stereo average from 48 kHz to 16 kHz. DMA timestamps align playback and capture,
with integer time lookup rather than assuming both I2S controllers stay in phase.
AEC frame/history storage and the TTS audio buffer live in PSRAM; queue/ISR and
DMA storage remain internal. Playback DMA resources are reserved before AEC starts.
AEC runs before high-pass, optional profile denoising, and gain/clipping. It uses
512-sample (32 ms) frames and 16 ms reference lookahead; frame assembly and lookahead
add latency. Missing/stale references bypass cancellation for that frame. Queue
losses discard incomplete microphone frames and are counted in diagnostics.

Start music at the troublesome volume and let it play for a few seconds. Capture:

```sh
python3 scripts/capture_aec.py DEVICE_IP /tmp/johan2-aec-music
```

This makes one 15-second request and saves the comparison WAV, four separate mono
WAVs, and `metrics.txt`. Leave the first three seconds to music alone, then speak
nearby and from your normal position. Avoid the wake word during recording so
Assist does not mute music or play a reply halfway through. Repeat with music off
using a new output directory. Then test real wake-word attempts separately at the
same volume and position; a WAV comparison alone cannot establish detection rates.

The direct endpoint is also available:

```sh
curl --fail --max-time 30 -D aec-headers.txt -o mic-aec.wav http://DEVICE_IP:8081/mic-aec.wav
```

`/mic-aec.wav` now defaults to mono final Assist input, rather than mixing a
playback reference and unprocessed audio into a normal listening test. Use
`?channel=before`, `?channel=reference`, `?channel=clean`, or `?channel=assist` for
one mono track. `?channel=all` returns the simultaneous 16 kHz, four-channel,
signed 16-bit analysis WAV (15 seconds, 1,920,000 audio bytes). The capture utility
requests `channel=all`, splits it into mono WAVs, and on macOS also exports individual
44.1 kHz AAC/M4A tracks for the Music app (a temporary resampled copy feeds
the macOS AAC encoder; the analysis WAV remains at 16 kHz). Do not audition the analysis WAV as a combined
mix to judge cancellation: it intentionally includes the original mic and music.
Channels in the analysis WAV are:

| Channel | Contents | Purpose |
| --- | --- | --- |
| 1 | Microphone before AEC, before gain | Original speaker bleed and voice |
| 2 | Time-aligned, downsampled playback reference | Check the music reference exists |
| 3 | AEC output, before gain and filtering | Compare with channel 1 at equal playback volume |
| 4 | Final Assist input | Hear the signal actually sent to HA |

Channel 4 can be louder because of gain and can have additional profile-denoiser
latency. Compare channels 1 and 3 for echo suppression and preservation of your
voice. The reference is a stereo average; strongly different/opposite-phase stereo
channels, external speaker delay, amplifier distortion and microphone overload
can limit cancellation. `JOHAN2_MIC_AEC_DELAY_MS` adds 0–150 ms reference delay. The diagnostic query
`delay_ms=N` changes it until another change or reboot; the capture task recreates
the learned filter once on change, and the endpoint waits two seconds before
recording. For example, `scripts/capture_aec.py DEVICE_IP /tmp/aec-25 --delay-ms 25`
compares a delayed speaker path without repeatedly flashing. Use measurements,
not speaker proximity alone, to choose a delay; Bluetooth/DSP speaker AUX paths
may still have processing latency. Metrics report the active delay.

`X-Mic-Metrics` adds `aec` (initialization succeeded), `aec_frames` (frames actually
processed by AEC), `aec_missing_samples`, `aec_reference_gaps`, `aec_reference_drops`, and
`aec_mic_drops`. Counters report changes during this capture. During continuous
music, expect many processed frames and zero drops; an initialized AEC with zero
processed frames means it was bypassed. Missing-reference counts can rise around
playback start/stop. Processing time includes reference resampling and AEC across
the complete frame; block intervals are normally about 32 ms with AEC enabled.
The Bose Mini installation measured about 30 ms of speaker-path delay. Its current
ignored local configuration starts at 25 ms, leaving a small causal margin for the
AEC filter. Separate music-only captures measured approximately 6.6 dB input/output
attenuation at 0 ms and 11–12 dB at 25–30 ms, with zero queue drops. These were
different music segments, not an identical-signal laboratory comparison, and do
not establish wake-word detection rates. DSP runs on core 1 with the hot capture/AEC
sources optimized; a prior music-on capture exposed CPU-0 idle starvation.

The endpoint returns 503 if AEC is disabled or failed initialization. Existing
`/mic.wav` and `/mic-compare.wav` remain available. The same temporary diagnostics
access and in-memory recording behavior described above applies.

Host checks (the AEC adapter test uses a DSP stub, not the vendor algorithm):

```sh
c++ -O2 -std=c++17 -Wall -Wextra -Werror -I main tests/playback_reference_test.cpp -o /tmp/johan2-playback-reference-test
/tmp/johan2-playback-reference-test
c++ -O2 -std=c++17 -Wall -Wextra -Werror -I main -I tests/aec_stubs tests/mic_aec_test.cpp main/mic_aec.cpp -o /tmp/johan2-mic-aec-test
/tmp/johan2-mic-aec-test
python3 tests/capture_aec_test.py
```

### Simultaneous raw/processed microphone comparison

`http://DEVICE_IP:8081/mic-compare.wav` starts a 15-second, 16 kHz stereo,
32-bit signed PCM recording. The left channel preserves each signed 32-bit I2S
word exactly as delivered by the driver (24-bit microphone payload left-aligned;
the low eight bits are not treated as microphone precision). The right channel
contains the exact Assist int16 samples multiplied by 65536, preserving their
level in 32-bit PCM. The two channels are captured in the same microphone task
iteration. The original `/mic.wav` endpoint remains mono 16-bit PCM.

The left channel has no firmware DC removal or gain. It will normally sound much
quieter than the right channel: gain 16 adds about 24 dB, while the current
fixed gain 4 adds about 12 dB. With automatic gain enabled the level difference
varies; consult the effective gain metadata. Compare
spectra after accounting for gain, the first-order DC filter (pole
255/256, approximately 10 Hz cutoff at 16 kHz), and the configurable rumble
high-pass filter (100 Hz by default). Both channels are downstream of
the microphone and I2S receiver; this comparison cannot on its own distinguish
acoustic, microphone hardware, power, or I2S faults. Response level/clipping
metrics still refer to the processed Assist channel.

Source review against ESP-IDF 6.0.1 confirms Philips timing with a one-bit shift,
32-bit slots, two physical slots per frame (64 BCLK cycles), left-slot receive,
and signed conversion without an unsigned intermediate. This matches the
[INMP441 I2S interface](https://www.invensense.com/wp-content/uploads/2015/02/INMP441.pdf),
assuming the physical module is an INMP441 and its L/R pin is grounded.
The conversion was extracted without changing its algorithm into `main/mic_pcm.h`.
Host checks cover signed polarity, saturation, constant-offset decay and zero
input; these checks do not verify physical timing or microphone quality:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -I main tests/mic_pcm_test.cpp -o /tmp/johan2-mic-pcm-test
/tmp/johan2-mic-pcm-test
```

### Microphone rumble filter

`JOHAN2_MIC_HIGHPASS_HZ` defaults to 100 Hz (range 0–300 Hz). A second-order
Butterworth high-pass runs in the microphone task after the existing DC removal
and before gain/saturation. At 100 Hz cutoff it attenuates 30 Hz by roughly 21 dB
and 50 Hz by roughly 12 dB; 500 Hz and 1 kHz remain within 0.2 dB in host tests.
It can reduce voice bass and does not repair microphone, contact or I2S faults.
Setting it to 0 restores the original DC-removal-only path for comparison.

Assist, `/mic.wav`, and the right channel of `/mic-compare.wav` all receive the
filtered PCM. The comparison WAV's left channel remains untouched I2S data.
`X-Mic-Metrics` includes `highpass_hz` alongside gain and clipping counts.
Verify audible quality and speech recognition on hardware after flashing.

### Experimental measured-profile noise suppression

`JOHAN2_MIC_PROFILE_DENOISE` enables a soft spectral suppressor calibrated from
user recordings mic-18 (noisy seconds 5–8 at HP100) and mic-27 (quiet seconds 3–9
at HP180), both at gain 16. It is disabled by default and enabled in the current
ignored local sdkconfig for a listening trial. No recordings are embedded or
committed; `main/mic_noise_profile.h` contains only 257 power values. Generation
corrects for the two high-pass responses and gain, targets only bins showing
more than 6 dB excess over the quiet reference, and leaves bins above 2 kHz alone.
The comparison is approximate: the two recordings were made at different times.

The suppressor uses a 512-point FFT, sqrt-Hann windows with 50% overlap, soft
power-based attenuation (maximum about 20 dB per targeted bin) and frequency/time
smoothing. Clear signals above the stored noise floor receive less attenuation.
It runs after HP/DC removal and before configurable gain and final clipping,
using fixed capture-task storage (about 20 KB), no per-block allocation or
networking, and a bounded FFT per 256 samples. Its 512-sample (32 ms) delay applies
to Assist, `/mic.wav`, and the processed comparison channel. The raw comparison
channel is untouched and leads the processed channel by 512 samples when enabled;
account for this delay before comparing waveforms. Turning the option off
restores the high-pass-only path with zero additional spectral-filter delay.

Diagnostic metadata includes `profile_denoise`, `delay_samples` and
`max_processing_us` (maximum measured DSP wall time per capture block during the
recording). Read-error counts and intervals do not establish DMA continuity;
a physical smoke test measured a maximum 7.928 ms DSP wall time for a 16 ms
block, with no reported read errors. Playback coexistence and broader load
conditions still require hardware checks. That uncontrolled recording contained
90 clipped output samples; suppression does not guarantee clipping-free audio.
This filter can attenuate weak speech or color voices and cannot restore audio
lost through an intermittent connection or already clipped in the microphone.

Host tests and the audition utility use the same C++ implementation as firmware:

```sh
c++ -O2 -std=c++17 -Wall -Wextra -Werror -I main tests/mic_denoise_test.cpp -o /tmp/johan2-mic-denoise-test
/tmp/johan2-mic-denoise-test
c++ -O2 -std=c++17 -Wall -Wextra -Werror -I main tools/filter_mic_pcm.cpp -o /tmp/johan2-filter-pcm
```

The utility accepts already HP/DC-filtered signed 16-bit mono PCM on stdin;
arguments are high-pass cutoff and recording gain. Output contains 512 trailing
flush samples. Remove its first 512 samples to align it to the input for audition
and level comparison. `tools/make_mic_noise_profile.py` (requires numpy) regenerates
the numeric header from the two original WAVs without copying source audio into
the project. In an offline trial on mic-18, the known noisy seconds 5–8 fell by
9.7 dB RMS and the louder seconds 0–4 fell by 0.8 dB. These measurements do not
establish speech recognition or perceptual quality.

### Accepted fixed-gain microphone baseline (2026-10-18)

The user accepted the filtered quiet recording and subsequently confirmed improved
music-on recording after reducing gain from 16 to 4. Reproduce this fixed-gain reference
through menuconfig or ignored local sdkconfig settings:

```ini
CONFIG_JOHAN2_MIC_GAIN=4
CONFIG_JOHAN2_MIC_HIGHPASS_HZ=180
CONFIG_JOHAN2_MIC_PROFILE_DENOISE=y
CONFIG_JOHAN2_MIC_DIAGNOSTICS=y
```

Repository defaults remain gain 16 / HP100 with profile suppression and HTTP
diagnostics disabled. Credentials and local sdkconfig are not committed. The
LED duplicate-frame change is part of the firmware; its contribution to the
listening improvement is unproven. Build with `./scripts/idf.sh build` after
applying local settings, then flash the intended device. Listening feedback does
not establish clipping-free operation at every music level or Assist recognition.

### Fixed microphone gain

Microphone PCM uses only the fixed `JOHAN2_MIC_GAIN` multiplier after noise
suppression. Automatic microphone gain control has been removed, and all HA
Assist requests explicitly disable HA automatic gain. `X-Mic-Metrics` reports
`agc=0` and the fixed effective gain during capture.

### HomeDash listening state and remote wake

Install `ha/johan2_assist.yaml` as `/config/packages/johan2_assist.yaml` on HA
(packages enabled), validate the configuration and load its helpers/scripts.
Enable `JOHAN2_HOMEDASH_SYNC` in Johan2 and `HOMEDASH_JOHAN2_ASSIST` in HomeDash;
both default off and are enabled in the current local configurations. Rebuild and
flash both devices. Existing HomeDash transcript posting is unchanged.

Johan2 publishes `input_select.johan2_assist_state`: `idle` means wake-word armed,
not command capture; `starting`, `listening`, `processing`, `speaking`, `error`
and `offline` describe the command lifecycle. Wake-word detection/STT start sets
listening, VAD/STT end sets processing, reply handling sets speaking, and completion
rearms idle. HomeDash renders listening only for the reported listening state,
clears an old transcript when a new capture begins, and uses its existing busy UI
for starting/processing/speaking. Proxy reconnects receive the current entity state.
HA and local UI watchdogs clear stale busy state after connection/device failures.
Johan2 also reasserts its current state every 15 seconds, so a helper that loads
after authentication or is reloaded later converges without a device reboot.

Both `script.johan2_assist_start` and the existing HomeDash `assist` remote command
start Johan2 STT directly, without a spoken wake word or a second HomeDash audio
pipeline. The package scripts press `input_button.johan2_assist_start/cancel`;
Johan2 uses a filtered `subscribe_entities` subscription to just these two helpers.
This works with a normal device account (custom event-bus subscriptions were
rejected by the deployed account). Initial/restored snapshots, duplicate button
timestamps and old subscription/run IDs cannot replay a wake. The Assist owner
task cancels the passive wake run with HA `unsubscribe_events` on its existing
authenticated socket, then starts at `stt`
with the same microphone, conversation and reply route. Wait for listening on
HomeDash before speaking; `starting` does not indicate active capture.

`script.johan2_assist_cancel` / `assist cancel` cancels a pending/listening command
and rearms wake detection. The UI button toggles cancel while active; repeated
non-UI start commands are ignored while busy. Already playing TTS is not stopped,
and completed HA actions are not undone. Optional duration arguments from the
legacy HomeDash capture path do not set Johan2's capture duration; HA VAD/timeout
controls it. HomeDash local mic-level/echo diagnostics remain local, and disabling
its remote mode restores the original HomeDash microphone Assist path.

Validation: protocol host tests and both ESP-IDF builds passed. Both devices were
flashed with verified hashes; start/listen/cancel passed through the direct and
existing HomeDash remote routes, and helper state recovery took 14.9 seconds.
The initial test cancelled immediately on listening and missed a user-reported
short failed capture. The follow-up keeps remote listening visibly on screen,
labels startup separately, and reports a failed capture for five seconds instead
of silently closing. Full spoken capture/reply cycles were observed in device
logs; the user confirmed that the revised flow works well.
