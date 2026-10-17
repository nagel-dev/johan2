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
is independent of the LED noise gate. `JOHAN2_MIC_GAIN` defaults to 8 and can be
adjusted if speech clips or is too quiet.

Music retains the coloured LED effect, with white microphone activity as before.
After wake-word detection, playback is reduced to one fifth of the current digital
volume until the command completes, then restored without changing the player
volume setting. There is no acoustic echo cancellation: loud speaker audio can
still impair wake-word detection. When `JOHAN2_HA_REPLY_PLAYER` is set, the pipeline
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
