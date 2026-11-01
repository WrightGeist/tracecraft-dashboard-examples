#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_RPM = 0,
    FLOW_GLOBAL_VARIABLE_SPEED = 1,
    FLOW_GLOBAL_VARIABLE_GEAR = 2,
    FLOW_GLOBAL_VARIABLE_FUEL_PCT = 3,
    FLOW_GLOBAL_VARIABLE_CAUTION = 4
};

// Native global variables

extern int32_t get_var_rpm();
extern void set_var_rpm(int32_t value);
extern const char *get_var_speed();
extern void set_var_speed(const char *value);
extern const char *get_var_gear();
extern void set_var_gear(const char *value);
extern int32_t get_var_fuel_pct();
extern void set_var_fuel_pct(int32_t value);
extern bool get_var_caution();
extern void set_var_caution(bool value);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/