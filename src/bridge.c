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

static void send_mouse_report(struct otg_bridge *b, uint8_t buttons,
                              int8_t dx, int8_t dy, int8_t vscroll,
                              int8_t hscroll)
{
    uint8_t report[AOA_MOUSE_REPORT_SIZE];
    report[0] = buttons;
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;
    report[3] = (uint8_t)vscroll;
    report[4] = (uint8_t)hscroll;
    if (!aoa_hid_send(&b->aoa, AOA_HID_ID_MOUSE, report, sizeof(report))) {
        LOG_WARN("failed to send mouse report\n");
    }
}

/*
 * Walk the device's relative pointer by (dx, dy), splitting the travel over
 * as many reports as needed since one report carries at most 127 per axis.
 * `buttons` is sent with every report, so a caller can keep a button released
 * for the whole move.
 */
static void mouse_travel(struct otg_bridge *b, int32_t dx, int32_t dy,
                         uint8_t buttons)
{
    while (dx != 0 || dy != 0) {
        int8_t sx = clamp_i8(dx);
        int8_t sy = clamp_i8(dy);
        send_mouse_report(b, buttons, sx, sy, 0, 0);
        dx -= sx;
        dy -= sy;
    }
}

/* Send a mouse report carrying the current button state. */
static void send_mouse_move(struct otg_bridge *b, int32_t dx, int32_t dy)
{
    /* A single report can only carry +/-127 per axis, and the server's cursor
     * can jump much further than that between two packets (a fast flick on a
     * high-poll-rate mouse easily spans >1000px).  Split the travel so no
     * motion is dropped, otherwise the device pointer silently falls behind
     * and eventually sticks to a screen edge. */
    mouse_travel(b, dx, dy, b->mouse_buttons);
}

/*
 * Force the device pointer to exactly (tx, ty) -- the equivalent of the
 * XWarpPointer() the real deskflow client does on enter (deskflow's
 * Client::enter() calls Screen::mouseMove() to place the local cursor on the
 * enter coordinates before any delta is applied).
 *
 * The AOA mouse is relative only, so there is no way to address an absolute
 * position directly.  Instead:
 *
 *   1. Travel by a full screen width/height towards the *minimum* edge.  Any
 *      starting point clamps onto 0, so afterwards the position is known for
 *      certain -- it is exactly (0, 0) regardless of where the pointer had
 *      been left, whether Android reset it, or whether it got stuck.
 *   2. Travel by the remaining offset to reach the target.  Both the target
 *      and the origin are inside the reported screen, so this leg never
 *      clamps and lands exactly.
 *
 * The buttons are held released for the whole warp so that entering the phone
 * mid-gesture cannot turn into a click or a drag smeared across the screen;
 * the previous state is restored afterwards.
 */
static void mouse_warp(struct otg_bridge *b, int32_t tx, int32_t ty)
{
    int32_t maxx = b->screen_w - 1;
    int32_t maxy = b->screen_h - 1;
    if (maxx < 0) {
        maxx = 0;
    }
    if (maxy < 0) {
        maxy = 0;
    }
    if (tx < 0) {
        tx = 0;
    } else if (tx > maxx) {
        tx = maxx;
    }
    if (ty < 0) {
        ty = 0;
    } else if (ty > maxy) {
        ty = maxy;
    }

    mouse_travel(b, -b->screen_w, -b->screen_h, 0); /* -> (0, 0), known good */
    mouse_travel(b, tx, ty, 0);                     /* -> (tx, ty) exactly  */

    if (b->mouse_buttons) {
        send_mouse_report(b, b->mouse_buttons, 0, 0, 0, 0);
    }
}

int otg_bridge_open(struct otg_bridge *b, const char *serial,
                    int32_t screen_w, int32_t screen_h)
{
    memset(b, 0, sizeof(*b));
    b->screen_w = screen_w > 0 ? screen_w : 1;
    b->screen_h = screen_h > 0 ? screen_h : 1;

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

    /* The server's cursor is absolute and clamped to the screen rectangle,
     * while the device pointer is relative and clamped to the same rectangle.
     * Differencing the absolute positions is only correct if both pointers
     * currently sit at the same spot, and on enter the device pointer is
     * wherever the last visit stranded it.  Put the device pointer on the
     * enter position first, so from here on the two advance together. */
    mouse_warp(b, x, y);

    b->mouse_x = x;
    b->mouse_y = y;
    b->have_mouse_pos = true;
    b->active = true;
    send_keyboard_report(b);
    LOG_DBG("enter at %d,%d mask=0x%04x\n", x, y, mask);
}

void otg_bridge_leave(struct otg_bridge *b)
{
    b->active = false;
    if (b->mouse_buttons) {
        /* Never leave a button stuck down on the device. */
        b->mouse_buttons = 0;
        send_mouse_report(b, 0, 0, 0, 0, 0);
    }
    if (b->have_mouse_pos) {
        /* mouse_x/mouse_y still hold the server's cursor position at the
         * moment the cursor left the screen (they are only cleared just
         * below), so log where it exited from. */
        LOG_DBG("leave at %d,%d (screen %dx%d)\n", b->mouse_x, b->mouse_y,
                b->screen_w, b->screen_h);
    } else {
        LOG_DBG("leave (cursor position unknown)\n");
    }
    b->have_mouse_pos = false;
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
    if (b->active) {
        send_mouse_report(b, b->mouse_buttons, 0, 0, 0, 0);
    }
}

void otg_bridge_mouse_move(struct otg_bridge *b, int16_t x, int16_t y)
{
    /* Absolute move: convert to a relative delta (AOA mouse is relative).
     * The delta base was placed on the enter position by mouse_warp(), and the
     * server clamps its cursor to the same rectangle the device clamps its
     * pointer to, so from here on the device pointer follows 1:1. */
    int32_t dx = 0;
    int32_t dy = 0;
    if (b->have_mouse_pos) {
        dx = (int32_t)x - b->mouse_x;
        dy = (int32_t)y - b->mouse_y;
    }
    b->mouse_x = x;
    b->mouse_y = y;
    b->have_mouse_pos = true;

    /* Drop anything outside the enter/leave window: the server only sends
     * motion to the active screen, but a packet may still be in flight when it
     * switches away. */
    if (!b->active || (dx == 0 && dy == 0)) {
        return;
    }
    send_mouse_move(b, dx, dy);
}

void otg_bridge_mouse_rel_move(struct otg_bridge *b, int16_t dx, int16_t dy)
{
    if (!b->active) {
        return;
    }
    /* The server does not advance its cursor for relative moves, so keep the
     * absolute base in step here; otherwise an absolute move arriving later
     * would be turned into a bogus delta. */
    if (b->have_mouse_pos) {
        b->mouse_x += dx;
        b->mouse_y += dy;
    }
    send_mouse_move(b, dx, dy);
}

void otg_bridge_mouse_wheel(struct otg_bridge *b, int16_t x, int16_t y)
{
    if (!b->active) {
        return;
    }
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
    send_mouse_report(b, b->mouse_buttons, 0, 0, vs, hs);
}
