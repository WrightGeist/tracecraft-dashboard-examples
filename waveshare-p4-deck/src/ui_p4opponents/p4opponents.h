#pragma once
// TraceCraft P4 deck — opponents / live-timing page (see p4opponents.cpp).
struct _lv_obj_t;
_lv_obj_t* p4opponents_setup();
void p4opponents_tick(bool waitingForData);
