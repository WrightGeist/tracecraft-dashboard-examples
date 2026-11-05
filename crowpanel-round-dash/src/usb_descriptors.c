// The USB device: a serial console, the TraceCraft dashboard interface (HID instance 0)
// and a gamepad with six buttons (HID instance 1). The name and IDs come from
// usb::setIdentity() in main.cpp; the serial number is the chip's own id.
#include <string.h>
#include "tusb.h"
#include "tracecraft/usb/idf.h"

static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Interface Association, so Windows groups the serial port's two interfaces as one.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    desc_device.idVendor  = tracecraft_usb_vid();
    desc_device.idProduct = tracecraft_usb_pid();
    return (uint8_t const *)&desc_device;
}

static uint8_t const desc_dashboard_report[] = { TRACECRAFT_HID_REPORT_DESC };

// Six buttons in one byte, and nothing else: the board has no analog controls.
static uint8_t const desc_gamepad_report[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x05,  // Usage (Gamepad)
    0xA1, 0x01,  // Collection (Application)
    0x05, 0x09,  //   Usage Page (Button)
    0x19, 0x01,  //   Usage Minimum (1)
    0x29, 0x06,  //   Usage Maximum (6)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x06,  //   Report Count (6)
    0x81, 0x02,  //   Input (Data,Var,Abs)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x02,  //   Report Count (2): pad the byte
    0x81, 0x03,  //   Input (Const,Var,Abs)
    0xC0         // End Collection
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    return instance == TRACECRAFT_HID_INSTANCE ? desc_dashboard_report : desc_gamepad_report;
}

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_DASHBOARD, ITF_GAMEPAD, ITF_TOTAL };

#define EP_CDC_NOTIF     0x81
#define EP_CDC_OUT       0x02
#define EP_CDC_IN        0x82
#define EP_DASHBOARD_OUT 0x03
#define EP_DASHBOARD_IN  0x83
#define EP_GAMEPAD_IN    0x84

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_HID_INOUT_DESCRIPTOR(ITF_DASHBOARD, 5, HID_ITF_PROTOCOL_NONE, sizeof(desc_dashboard_report),
                             EP_DASHBOARD_OUT, EP_DASHBOARD_IN, CFG_TUD_HID_EP_BUFSIZE, 1),
    TUD_HID_DESCRIPTOR(ITF_GAMEPAD, 6, HID_ITF_PROTOCOL_NONE, sizeof(desc_gamepad_report),
                       EP_GAMEPAD_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static uint16_t desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    const char *str;
    switch (index) {
        case 0:  desc_str[1] = 0x0409; desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4); return desc_str;
        case 1:  str = tracecraft_usb_manufacturer(); break;
        case 2:  str = tracecraft_usb_product(); break;
        case 3:  str = tracecraft_usb_serial(); break;
        case 4:  str = "Console"; break;
        case 5:  str = "Dashboard"; break;
        case 6:  str = "Gamepad"; break;
        default: return NULL;
    }
    size_t n = strlen(str);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) desc_str[1 + i] = (uint8_t)str[i];
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
