# CrowPanel round dash and encoder gamepad

Two arcs, brake bias and engine map, drawn from what the sim reports. The rotary
encoder and its center button are a gamepad on the same USB cable, so you bind them in
the sim like any other controller.

**Hardware:** Elecrow CrowPanel 1.28" round, 240x240 GC9A01, with the rotary encoder.

## Build and flash

Install [PlatformIO](https://platformio.org), open this folder, then:

```
pio run -t upload
```

PlatformIO downloads the TraceCraft libraries (TraceCraftCore and TraceCraftDisplay
0.1.14 or later; the gamepad needs 0.1.14) and everything else named in
`platformio.ini`. With the [TraceCraft Recorder](https://tracecraft.app/recorder)
running, plug the board in: it finds the device, asks which channels it wants, and
streams them.

A running dashboard flashes hands-free. A first flash, or a board whose firmware does not
run, still needs BOOT held while you tap RST.

This folder is a complete project. Copy it anywhere and make it yours.

## The gamepad

| Button | Input |
|---|---|
| 1 | one clockwise detent: a press, then a release |
| 2 | one counterclockwise detent: a press, then a release |
| 3 | the center button, held while you hold it |

Open the sim's controls, pick the binding (brake bias up and down, say), and turn the
knob. Each detent is a separate press of about 50 ms with a 50 ms gap, so turn at a
steady pace: up to twelve detents queue, turning back cancels queued ones, and a faster
spin drops the rest (the USB console says how many).

Three buttons drive one pair of up/down bindings. The knob does not switch between
brake bias and engine map by itself; bind button 3 to something useful, or rebind the
pair. The arcs move only when the sim changes the value, so what you see is what the
car is running.

If the knob counts the wrong way, swap `kEncA` and `kEncB` in `src/main.cpp`.

## USB identity

The board is three USB functions on one cable: the TraceCraft telemetry interface,
the gamepad, and a serial console. It enumerates as `303A:4D02`, not the plain
dashboard's `303A:4D01`: Windows remembers a device's interfaces by VID and PID, and
this one has more. In Windows, "Set up USB game controllers" (`joy.cpl`) lists it as
"TraceCraft Round Dash", with two axes that stay centered: Windows' Properties page
expects a controller to have some.

## Checking it on hardware

1. Flash, and open `joy.cpl`: the controller is listed and Properties opens, with the
   X/Y cross centered. Turn the knob each way and press it: buttons 1, 2 and 3 light.
2. Start the recorder and a sim: the arcs show the car's brake bias and engine map.
3. With the sim bound, turn the knob: the value in the sim moves one step per detent,
   and the arc follows.
4. Unplug and replug mid-turn, and restart the recorder: no button stays lit, and the
   arcs come back.
