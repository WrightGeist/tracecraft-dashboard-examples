# Raspberry Pi Pico shift light

One green LED as the entire display, ramping brightness and blink rate with RPM. The template for any single-channel actuator: servo angle, fan duty, haptic amplitude.

**Hardware:** Raspberry Pi Pico. Not the Pico W, whose LED hangs off the wireless chip. Hold BOOTSEL while plugging in, then upload or copy `firmware.uf2` to the drive.

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
