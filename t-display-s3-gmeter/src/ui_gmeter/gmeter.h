#pragma once
#include <lvgl.h>

// T-Display S3 (320x170) — the G-dot: a 2D friction-circle plot. X = lateral
// (full width, high fidelity), Y = longitudinal (braking up / accel down,
// compressed — a deliberate trade). The dot is the driver's head in the
// inertia frame — it goes where the forces throw you. Smoothed dot with a
// fading trail, corner LAT/LON readouts, session-peak ghosts on both axes.
void gmeter_setup(lv_obj_t* scr);
void gmeter_tick(bool waitingForData);

// Feed the on-screen link-rate readout.
//
// This board is the proof rig for "how fast can a device actually be fed": it
// declares a handful of channels, so its frame is ONE 55-byte USB report and its
// wire ceiling is ~900 Hz, versus ~72 Hz for a board declaring 150 channels. The number on screen is measured on the DEVICE -- frames decoded here,
// not frames the host believes it sent.
//
// `framesApplied` is Dashboard::framesApplied() (frames actually decoded) and
// `seq` is HidTransport::frameSeq(). Both are needed and neither is sufficient: a
// slow link and a lossy one look identical if you only count arrivals, and SEQ is
// host-assigned per frame, so a gap is exactly how many frames were sent that
// never completed here.
//
// Call it every loop; it maintains its own one-second window.
void gmeter_rate(uint32_t framesApplied, uint8_t seq);
