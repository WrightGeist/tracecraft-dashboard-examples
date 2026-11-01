# CrowPanel round setting tweaker

Two arcs, brake bias and engine map. The rotary encoder adjusts the active gauge and the center button switches between them.

**Hardware:** Elecrow CrowPanel 1.28" round, 240x240 GC9A01, with the rotary encoder.

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
