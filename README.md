# TraceCraft dashboard examples

Example USB dashboards and peripherals for [TraceCraft](https://tracecraft.app): small
screens, shift lights and button boxes that show live sim-racing telemetry. Plug one in
and the [TraceCraft Recorder](https://tracecraft.app/recorder) finds it, asks which
channels it wants, and streams them, with no setup on the PC.

Each folder is a complete, standalone [PlatformIO](https://platformio.org) project. Copy
one, change it, and it is yours.

| Project | What | Board |
|---|---|---|
| [channel-monitor](channel-monitor/) | Channel monitor | Lilygo T4-S3, Lilygo T-Display S3, Elecrow CrowPanel 1.28" round, or Waveshare ESP32-P4-WIFI6-Touch-LCD-5 |
| [t4-s3-eez-dash](t4-s3-eez-dash/) | T4-S3 EEZ dashboard | Lilygo T4-S3 AMOLED, 450x600 |
| [t4-s3-deck](t4-s3-deck/) | T4-S3 deck | Lilygo T4-S3 AMOLED, 450x600, with the CST226 touch panel |
| [t-display-s3-gmeter](t-display-s3-gmeter/) | T-Display S3 G-meter | Lilygo T-Display S3, 320x170 ST7789 |
| [t-display-s3-amoled-dash](t-display-s3-amoled-dash/) | T-Display S3 AMOLED driver dash | Lilygo T-Display S3 AMOLED, 536x240 RM67162 |
| [crowpanel-round-dash](crowpanel-round-dash/) | CrowPanel round dial: a dashboard and a gamepad on one cable (ESP-IDF) | Elecrow CrowPanel 1.28" round, 240x240 GC9A01, with the rotary encoder |
| [waveshare-p4-deck](waveshare-p4-deck/) | Waveshare P4 deck | Waveshare ESP32-P4-WIFI6-Touch-LCD-5, 720x1280 MIPI-DSI |
| [headless-serial-console](headless-serial-console/) | Headless serial console | Any ESP32-S3 board |
| [trinket-shift-light](trinket-shift-light/) | Trinket M0 shift light | Adafruit Trinket M0 |
| [pico-shift-light](pico-shift-light/) | Raspberry Pi Pico shift light | Raspberry Pi Pico |
| [macropad-dash](macropad-dash/) | MacroPad gamepad and dash | Adafruit MacroPad RP2040 |

## Build one

```
cd t-display-s3-gmeter
pio run -t upload
```

PlatformIO downloads everything a project names in its `platformio.ini`, including the two
TraceCraft libraries.

## Support

These are starting points, offered as they are. They are not products, and we do not support
specific boards. They build on every push here, but a board's screen and pins may need
adjusting for your revision. How to build on the libraries:
https://tracecraft.app/developers

## License

The examples are MIT: see [LICENSE](LICENSE). Some parts keep their own licenses, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The TraceCraft libraries are not in this repository and are not MIT. `TraceCraftCore` and
`TraceCraftDisplay` are downloaded when you build, under the TraceCraft SDK License that comes
with them. What you build with them is yours to sell or share. The TraceCraft name and logo
are not licensed by this repository: you may say your device works with TraceCraft, but not
that TraceCraft made or endorses it.
