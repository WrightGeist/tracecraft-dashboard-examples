idf_component_register(
    SRCS
        "main.cpp"
        "usb_descriptors.c"
        "Dialpad.cpp"
        "Display.cpp"
        "fonts/geist_sans_14.c"
        "fonts/geist_sans_16.c"
        "fonts/geist_sans_20.c"
        "fonts/geist_sans_40.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES tinyusb driver esp_lcd esp_lcd_gc9a01 esp_lvgl_port lvgl)

# The fonts include <lvgl.h> by its short name.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# The raw TinyUSB component reads tusb_config.h from this folder.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
