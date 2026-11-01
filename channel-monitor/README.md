# Channel monitor

Every channel the recorder sends, as a scrolling list of live values. The tool to reach for when bringing up a new board: it proves USB and the panel, and shows what the host is streaming.

**Hardware:** Lilygo T4-S3, Lilygo T-Display S3, Elecrow CrowPanel 1.28" round, or Waveshare ESP32-P4-WIFI6-Touch-LCD-5. Pick the env for your board: `pio run -e t_display_s3 -t upload`.

## Build and flash

Install [PlatformIO](https://platformio.org), open this folder, then:

```
pio run -t upload
```

PlatformIO downloads the TraceCraft libraries and everything else named in
`platformio.ini`. With the [TraceCraft Recorder](https://tracecraft.app/recorder)
running, plug the board in: it finds the device, asks which channels it wants, and
streams them.

This folder is a complete project. Copy it anywhere and make it yours.
