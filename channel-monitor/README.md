# Channel monitor

The SDK 0.1.13 USB channel catalog in two selectable groups, as a scrolling list of live values. The tool to reach for when bringing up a new board: it proves USB and the panel, and shows what the host is streaming.

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

## Channel groups

The USB registry uses 8-bit channel indices, so the complete catalog cannot fit in
one firmware image. `monitor.channels.json` contains the first 175 channels in
manifest order; `monitor-extended.channels.json` contains the remainder. Together
they cover every 0.1.13 manifest channel except those documented as local-dashboard-only
(and `voice_active`, which Recorder does not supply to USB devices).

Set `[monitor] channels = monitor-extended.channels.json` in `platformio.ini` to
select the second group for any board, then rebuild/upload. A ready-made
`t_display_s3_extended` environment also builds that group. These are build-time
groups, not simultaneously subscribed views. Missing or unsupported values show `-`;
a vocabulary entry does not guarantee that a particular sim/car supplies it.
