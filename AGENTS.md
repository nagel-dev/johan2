# Johan2 firmware

- Target ESP32-S3 N16R8 with PCM5102A: BCLK 6, LRCK 5, DOUT 4.
- Use ESP-IDF 6.0.1; run `./scripts/idf.sh build` after firmware changes.
- Credentials belong only in ignored local sdkconfig files; never log them.
- Keep third-party components managed and pinned. Do not edit managed_components.
- Keep ISR work bounded; do not call Sendspin, allocate, log, or write NVS in an ISR.
- Preserve the distinction between compilation and physical playback verification.
- Record substantive changes in CHANGES.md.
