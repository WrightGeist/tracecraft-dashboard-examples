# Headless serial console

No display and no LVGL: the USB telemetry client printing live channels to the serial console. The template for anything without a screen: swap the print loop for an SD logger, a servo, a CAN bus.

**Hardware:** Any ESP32-S3 board. On a board with a screen, the screen just stays dark.

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
