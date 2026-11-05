#pragma once
#include <stdint.h>

// The CrowPanel's rotary encoder and dial button.
//
// Each detent of the encoder presses a gamepad button for a moment, and which pair of
// buttons depends on the screen showing: buttons 1 and 2 on the first screen, 3 and 4 on
// the second, 5 and 6 on the third. Bind them in the sim (brake bias down and up, say),
// and one knob is six controls. The dial button changes the screen; it is not a gamepad
// button.

// Set up the pins and the encoder interrupt. Call once.
void dialpad_begin();

// Which screen is showing (0-2), so the encoder presses that screen's pair. A press
// already under way keeps its button.
void dialpad_set_page(uint8_t page);

// True once for each press of the dial button.
bool dialpad_take_press();

// The gamepad buttons held right now: bit 0 is button 1.
uint8_t dialpad_buttons();
