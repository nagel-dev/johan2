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
