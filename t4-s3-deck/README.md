# T4-S3 deck

Four pages you swipe between on the touch panel: the EEZ dashboard, tires and brakes, performance, and a sector badge. Every page keeps ticking off-screen.

**Hardware:** Lilygo T4-S3 AMOLED, 450x600, with the CST226 touch panel.

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
