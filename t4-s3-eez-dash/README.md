# T4-S3 EEZ dashboard

A dashboard designed visually in EEZ-Studio: open `ui/tracecraft-dash.eez-project`, bind a widget to a channel, build, flash. The project's global variables are its channel list.

**Hardware:** Lilygo T4-S3 AMOLED, 450x600.

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
