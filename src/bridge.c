/*
 * bridge.c - see bridge.h.
 *
 * The HID report descriptors and encoding below are copied verbatim (with
 * permission, Apache-2.0) from scrcpy:
 *   - app/src/hid/hid_keyboard.c  (SC_HID_KEYBOARD_REPORT_DESC and report layout)
 *   - app/src/hid/hid_mouse.c     (SC_HID_MOUSE_REPORT_DESC and report layout)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bridge.h"

#include <stdio.h>
#include <string.h>

#include "keymap.h"

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_DBG(...)  fprintf(stderr, "DEBUG: " __VA_ARGS__)

#define HID_KEYBOARD_MAX_KEYS 6

/* Deskflow wheel deltas are in units of 120 per notch (PlatformScreen.h
 * s_scrollDelta), matching the Windows WHEEL_DELTA convention. */
#define WHEEL_DELTA 120

/* scrcpy SC_HID_KEYBOARD_REPORT_DESC (8-byte report: 1 modifier + 1 reserved
 * + 6 key slots). */
static const uint8_t KEYBOARD_REPORT_DESC_FULL[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /* Usage Page (Key Codes) */
    0x19, 0xE0,       /* Usage Minimum (224) */
    0x29, 0xE7,       /* Usage Maximum (231) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x08,       /* Report Count (8) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute): modifiers */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x01,       /* Input (Constant): reserved byte */
    0x05, 0x08,       /* Usage Page (LEDs) */
    0x19, 0x01,       /* Usage Minimum (1) */
    0x29, 0x05,       /* Usage Maximum (5) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x05,       /* Report Count (5) */
    0x91, 0x02,       /* Output (Data, Variable, Absolute): LEDs */
    0x75, 0x03,       /* Report Size (3) */
    0x95, 0x01,       /* Report Count (1) */
    0x91, 0x01,       /* Output (Constant): LED padding */
    0x05, 0x07,       /* Usage Page (Key Codes) */
    0x19, 0x00,       /* Usage Minimum (0) */
    0x29, 0x65,       /* Usage Maximum (101) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x65,       /* Logical Maximum (101) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x06,       /* Report Count (6) */
    0x81, 0x00,       /* Input (Data, Array): keys */
    0xC0              /* End Collection */
};

/* scrcpy SC_HID_MOUSE_REPORT_DESC (5-byte report: buttons + X + Y + wheel +
 * horizontal scroll). */
static const uint8_t MOUSE_REPORT_DESC[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x01,       /* Usage (Pointer) */
    0xA1, 0x00,       /* Collection (Physical) */
    0x05, 0x09,       /* Usage Page (Buttons) */
    0x19, 0x01,       /* Usage Minimum (1) */
    0x29, 0x05,       /* Usage Maximum (5) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x95, 0x05,       /* Report Count (5) */
    0x75, 0x01,       /* Report Size (1) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute): 5 buttons */
    0x95, 0x01,       /* Report Count (1) */
    0x75, 0x03,       /* Report Size (3) */
    0x81, 0x01,       /* Input (Constant): padding */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x30,       /* Usage (X) */
    0x09, 0x31,       /* Usage (Y) */
    0x09, 0x38,       /* Usage (Wheel) */
    0x15, 0x81,       /* Logical Minimum (-127) */
    0x25, 0x7F,       /* Logical Maximum (127) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x03,       /* Report Count (3) */
    0x81, 0x06,       /* Input (Data, Variable, Relative): X, Y, Wheel */
    0x05, 0x0C,       /* Usage Page (Consumer Page) */
    0x0A, 0x38, 0x02, /* Usage (AC Pan) */
    0x15, 0x81,       /* Logical Minimum (-127) */
    0x25, 0x7F,       /* Logical Maximum (127) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x06,       /* Input (Data, Variable, Relative): AC Pan */
    0xC0,             /* End Collection (Physical) */
    0xC0              /* End Collection (Application) */
};

static int8_t clamp_i8(int32_t v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return (int8_t)v;
}

/* Build and send the full keyboard report (mirrors
 * sc_hid_keyboard_generate_input_from_key). */
static void send_keyboard_report(struct otg_bridge *b)
{
    uint8_t report[AOA_KEYBOARD_REPORT_SIZE];
    memset(report, 0, sizeof(report));

    report[0] = b->hid_mods;
    report[1] = 0; /* reserved */

    int n = 0;
    bool phantom = false;
    for (int i = 0; i < 0x66; ++i) {
        if (b->keys[i]) {
            if (n >= HID_KEYBOARD_MAX_KEYS) {
                phantom = true;
                break;
            }
            report[2 + n] = (uint8_t)i;
            ++n;
        }
    }
    if (phantom) {
        /* rollover / phantom state */
        for (int i = 0; i < HID_KEYBOARD_MAX_KEYS; ++i) {
            report[2 + i] = 0x01;
        }
    }

    if (!aoa_hid_send(&b->aoa, AOA_HID_ID_KEYBOARD, report,
                      sizeof(report))) {
        LOG_WARN("failed to send keyboard report\n");
    }
}

static void send_mouse_report(struct otg_bridge *b, int8_t dx, int8_t dy,
                              int8_t vscroll, int8_t hscroll)
{
    uint8_t report[AOA_MOUSE_REPORT_SIZE];
    report[0] = b->mouse_buttons;
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;
    report[3] = (uint8_t)vscroll;
    report[4] = (uint8_t)hscroll;
    if (!aoa_hid_send(&b->aoa, AOA_HID_ID_MOUSE, report, sizeof(report))) {
        LOG_WARN("failed to send mouse report\n");
    }
}

int otg_bridge_open(struct otg_bridge *b, const char *serial)
{
    memset(b, 0, sizeof(*b));

    if (!aoa_hid_init(&b->aoa)) {
        return -1;
    }
    if (!aoa_hid_open(&b->aoa, serial)) {
        aoa_hid_destroy(&b->aoa);
        return -1;
    }
    if (!aoa_hid_register(&b->aoa, AOA_HID_ID_KEYBOARD,
                          KEYBOARD_REPORT_DESC_FULL,
                          sizeof(KEYBOARD_REPORT_DESC_FULL))) {
        LOG_ERR("could not register AOA keyboard\n");
        aoa_hid_destroy(&b->aoa);
        return -1;
    }
    if (!aoa_hid_register(&b->aoa, AOA_HID_ID_MOUSE,
                          MOUSE_REPORT_DESC, sizeof(MOUSE_REPORT_DESC))) {
        LOG_ERR("could not register AOA mouse\n");
        aoa_hid_destroy(&b->aoa);
        return -1;
    }
    return 0;
}

void otg_bridge_close(struct otg_bridge *b)
{
    aoa_hid_destroy(&b->aoa);
}

bool otg_bridge_check(struct otg_bridge *b)
{
    return aoa_hid_poll(&b->aoa);
}

void otg_bridge_enter(struct otg_bridge *b, int16_t x, int16_t y, uint16_t mask)
{
    /* Seed modifier state from the server's reported active-modifier mask;
     * subsequent modifier key events will refine it. */
    b->hid_mods = modmask_to_hid(mask);
    b->mouse_x = x;
    b->mouse_y = y;
    b->have_mouse_pos = true;
    send_keyboard_report(b);
    LOG_DBG("enter at %d,%d mask=0x%04x\n", x, y, mask);
}

void otg_bridge_leave(struct otg_bridge *b)
{
    b->mouse_buttons = 0;
    b->have_mouse_pos = false;
    LOG_DBG("leave\n");
}

void otg_bridge_key(struct otg_bridge *b, uint32_t keyid, bool down)
{
    int usage = keyid_to_hid(keyid);
    if (usage < 0) {
        LOG_DBG("unmapped key 0x%04x\n", keyid);
        return;
    }

    uint8_t mod_bit = 0;
    if (hid_usage_is_modifier((uint8_t)usage, &mod_bit)) {
        if (down) {
            b->hid_mods |= mod_bit;
        } else {
            b->hid_mods &= (uint8_t)~mod_bit;
        }
    } else if (usage <= 0x65) {
        b->keys[usage] = down;
    } else {
        return; /* out of report range */
    }

    send_keyboard_report(b);
}

void otg_bridge_mouse_button(struct otg_bridge *b, uint8_t button, bool press)
{
    /* deskflow ButtonID -> HID button bit
     * kButtonLeft=1, Middle=2, Right=3, Extra0=4, Extra1=5. */
    uint8_t bit;
    switch (button) {
        case 1: bit = 1 << 0; break; /* left */
        case 2: bit = 1 << 2; break; /* middle */
        case 3: bit = 1 << 1; break; /* right */
        case 4: bit = 1 << 3; break; /* extra0 */
        case 5: bit = 1 << 4; break; /* extra1 */
        default: return;
    }
    if (press) {
        b->mouse_buttons |= bit;
    } else {
        b->mouse_buttons &= (uint8_t)~bit;
    }
    send_mouse_report(b, 0, 0, 0, 0);
}

void otg_bridge_mouse_move(struct otg_bridge *b, int16_t x, int16_t y)
{
    /* Absolute move: convert to a relative delta (AOA mouse is relative). */
    if (b->have_mouse_pos) {
        int32_t dx = (int32_t)x - b->mouse_x;
        int32_t dy = (int32_t)y - b->mouse_y;
        send_mouse_report(b, clamp_i8(dx), clamp_i8(dy), 0, 0);
    }
    b->mouse_x = x;
    b->mouse_y = y;
    b->have_mouse_pos = true;
}

void otg_bridge_mouse_rel_move(struct otg_bridge *b, int16_t dx, int16_t dy)
{
    send_mouse_report(b, clamp_i8(dx), clamp_i8(dy), 0, 0);
}

void otg_bridge_mouse_wheel(struct otg_bridge *b, int16_t x, int16_t y)
{
    /* Mirror scrcpy's sc_hid_mouse_generate_input_from_scroll: accumulate
     * fractional scroll and emit an event once a whole notch accumulates. */
    b->residual_hscroll += (float)x / WHEEL_DELTA;
    b->residual_vscroll += (float)y / WHEEL_DELTA;

    int8_t hs = clamp_i8((int32_t)b->residual_hscroll);
    int8_t vs = clamp_i8((int32_t)b->residual_vscroll);
    b->residual_hscroll -= hs;
    b->residual_vscroll -= vs;

    if (hs == 0 && vs == 0) {
        return;
    }
    send_mouse_report(b, 0, 0, vs, hs);
}
