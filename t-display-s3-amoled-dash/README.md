# T-Display S3 AMOLED driver dash

Tire temperature (the whole screen colored to the tire's operating window, flashing when out of range), pit limiter, fuel and performance screens. Tap the panel to page.

**Hardware:** Lilygo T-Display S3 AMOLED, 536x240 RM67162.

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
