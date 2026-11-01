# T-Display S3 G-meter

A lateral-first G-meter: a friction-circle plot with a smoothed dot, a fading trail, corner readouts and session-peak ghosts. Two channels, so one USB report per frame.

**Hardware:** Lilygo T-Display S3, 320x170 ST7789.

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
