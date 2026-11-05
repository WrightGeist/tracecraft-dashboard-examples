# CrowPanel round dial

A TraceCraft dashboard and a game controller on one USB cable. The round screen shows
brake bias, TC, ABS and engine map as the sim reports them; the rotary encoder presses
gamepad buttons you bind in the sim, so turning the knob changes the setting and the dial
follows.

**Hardware:** Elecrow CrowPanel 1.28" round, 240x240 GC9A01, with the rotary encoder.

## Controls

| Screen | Shows | Encoder left / right |
|---|---|---|
| 1 | Brake bias, TC and ABS | Buttons 1 / 2 |
| 2 | Engine map | Buttons 3 / 4 |
| 3 | Brake bias, large | Buttons 5 / 6 |

Press the dial to change screen. In the sim's controls, bind the buttons to what each
screen shows: brake bias down and up to 1 and 2, for example. Windows lists the board in
its game controllers as "TraceCraft Round Dial".

The TC and ABS dials light up while the system is working. A sim that does not publish
engine map shows `--`.

## Build and flash

Install [PlatformIO](https://platformio.org), open this folder, then:

```
pio run -t upload
```

Run it from PowerShell or cmd on Windows, not Git Bash: ESP-IDF refuses an MSYS shell.
The first build downloads and compiles ESP-IDF and takes a while. A first flash needs the
board in download mode: hold BOOT, tap RST, release BOOT.

With the [TraceCraft Recorder](https://tracecraft.app/recorder) running, plug the board
in: the recorder finds it, asks for its channels and streams them.

## Why ESP-IDF

The other ESP32 examples are Arduino projects. On Arduino, every USB HID device on an
ESP32 shares one interface, and a TraceCraft dashboard needs one to itself, so a board is
a dashboard or a controller, never both. On ESP-IDF the project describes its own USB
device ([`src/usb_descriptors.c`](src/usb_descriptors.c),
[`src/tusb_config.h`](src/tusb_config.h)) and gives the dashboard and the gamepad an
interface each, with a serial console beside them. The TraceCraft libraries are the same
ones the Arduino examples use.

To add a control, widen the gamepad's report descriptor in `usb_descriptors.c` and set the
bit in `Dialpad.cpp`. A device that gains an interface needs a new product ID
(`kProductId` in `main.cpp`), because Windows remembers a device's interfaces by its IDs.

This folder is a complete project. Copy it anywhere and make it yours.
