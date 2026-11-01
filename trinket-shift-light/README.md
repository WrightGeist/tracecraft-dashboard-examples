# Trinket M0 shift light

A shift light on the board's single DotStar, ramping color with RPM. Three channels on a 32 KB SAMD21, the smallest part the libraries run on.

**Hardware:** Adafruit Trinket M0. Double-tap RST for the UF2 bootloader, then upload.

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
