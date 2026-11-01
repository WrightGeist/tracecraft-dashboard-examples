# MacroPad gamepad and dash

Twelve keys and an encoder as a USB game controller, and the 128x64 OLED as a telemetry dash: the driver's cockpit dials in a 3x4 grid, one page per sim. A key selects its cell, and the encoder taps that cell's increase and decrease buttons.

**Hardware:** Adafruit MacroPad RP2040. Hold BOOTSEL while plugging in, then upload.

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
