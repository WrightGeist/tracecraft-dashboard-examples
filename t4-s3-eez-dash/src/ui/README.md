# src/ui/ — EEZ-Studio export (generated)

This folder is filled in by EEZ-Studio's **Build** step from
`ui/tracecraft-dash.eez-project` (its destination folder is set to `../src/ui`).

It will contain `ui.h`, `ui.c`, `screens.{h,c}`, `vars.h`, `styles.{h,c}`,
`fonts.*`, etc. — the LVGL C code for your dashboard.

Don't hand-edit these files; re-Build in EEZ-Studio instead. The matching
`get_var_<name>()` implementations are generated into the build directory from the
project's global variables, by the TraceCraftCore library's build step.

Build with the EEZ UI:  `pio run -e t4_s3_eez -t upload`
