# Johan2 firmware

- Target ESP32-S3 N16R8 with PCM5102A: BCLK 6, LRCK 5, DOUT 4.
- Use ESP-IDF 6.0.1; run `./scripts/idf.sh build` after firmware changes.
- Always build via `./scripts/idf.sh`, not `idf.py`: it sets `safe.bareRepository=all` for its process, because a global `safe.bareRepository=explicit` breaks the component manager (`git config --get remote.origin.url` fails). Do not change global git config or delete caches to work around it.
- Credentials belong only in ignored local sdkconfig files; never log them.
- Keep third-party components managed and pinned. Do not edit managed_components.
- Keep ISR work bounded; do not call Sendspin, allocate, log, or write NVS in an ISR.
- Preserve the distinction between compilation and physical playback verification.
- Record substantive changes in CHANGES.md.
