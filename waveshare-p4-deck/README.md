# Waveshare P4 deck

Eight pages in one firmware, swiped between: race dash, timing, weather, opponents, system, race focus, frame rate and g-force. Taps dim or brighten the backlight. The deck asks the recorder for one page's channels at a time.

**Hardware:** Waveshare ESP32-P4-WIFI6-Touch-LCD-5, 720x1280 MIPI-DSI. Telemetry runs over the board's USB-OTG Type-C port; the other Type-C is UART only.

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
