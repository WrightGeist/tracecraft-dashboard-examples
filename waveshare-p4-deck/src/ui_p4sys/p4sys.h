#pragma once
// TraceCraft P4 deck — SYSTEM diagnostics page (see p4sys.cpp). Host-PC vitals
// (CPU / RAM / GPU / VRAM) as rolling history traces, from the recorder's sys_*
// channels. Live-only (never recorded).
struct _lv_obj_t;
_lv_obj_t* p4sys_setup();
void p4sys_tick(bool waitingForData);
