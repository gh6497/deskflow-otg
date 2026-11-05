/*
 * bridge.c - see bridge.h.
 *
 * The keyboard and relative mouse descriptors below are copied verbatim (with
 * permission, Apache-2.0) from scrcpy:
 *   - app/src/hid/hid_keyboard.c  (SC_HID_KEYBOARD_REPORT_DESC and report layout)
 *   - app/src/hid/hid_mouse.c     (SC_HID_MOUSE_REPORT_DESC and report layout)
 * The absolute tablet follows Android's TouchInputMapper::dispatchPointerStylus
 * and Linux drivers/hid/hid-input.c (Digitizer + Tip Switch + In Range).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "bridge.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "keymap.h"

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_DBG(...)  fprintf(stderr, "DEBUG: " __VA_ARGS__)

#define HID_KEYBOARD_MAX_KEYS 6
#define HID_ABSOLUTE_MAX 32767

/* Deskflow wheel deltas are in units of 120 per notch (PlatformScreen.h
 * s_scrollDelta), matching the Windows WHEEL_DELTA convention. */
#define WHEEL_DELTA 120
#define UNLOCK_WAKE_DELAY_MS 800
#define UNLOCK_SWIPE_STEPS 12
#define UNLOCK_SWIPE_INTERVAL_MS 10
#define UNLOCK_PIN_DELAY_MS 500

enum {
    UNLOCK_PHASE_NONE,
    UNLOCK_PHASE_WAIT_SWIPE,
    UNLOCK_PHASE_SWIPE,
    UNLOCK_PHASE_WAIT_PIN,
};

static bool usb_register_report(void *ctx, uint16_t id, const uint8_t *desc,
                                uint16_t size)
{
    return aoa_hid_register(ctx, id, desc, size);
}

static bool usb_send(void *ctx, uint16_t id, const uint8_t *data, uint16_t size)
{
    return aoa_hid_send(ctx, id, data, size);
}

static bool usb_poll(void *ctx)
{
    return aoa_hid_poll(ctx);
}

static bool usb_ready(void *ctx)
{
    return !aoa_hid_is_disconnected(ctx);
}

static void usb_close(void *ctx)
{
    aoa_hid_destroy(ctx);
}

static const struct otg_hid_transport_ops USB_TRANSPORT = {
    .register_report = usb_register_report,
    .send = usb_send,
    .poll = usb_poll,
    .ready = usb_ready,
    .close = usb_close,
};

/* scrcpy SC_HID_KEYBOARD_REPORT_DESC (8-byte report: 1 modifier + 1 reserved
 * + 6 key slots). Keep the F13..F24 range for keyboard compatibility and
 * existing BLE descriptor caches; navigation uses standard Meta shortcuts. */
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
    0x29, 0x73,       /* Usage Maximum (115) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x73,       /* Logical Maximum (115) */
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

/*
 * Absolute tablet: flags + buttons + uint16 LE X/Y + wheel + pan.
 * Android's normal mouse mapper only handles REL_X/Y and accelerates them.
 * A Digitizer application sets INPUT_PROP_POINTER; Tip Switch and In Range
 * provide BTN_TOUCH and BTN_TOOL_PEN, selecting absolute stylus positioning
 * with a visible pointer and hover instead of relative touchpad gestures.
 * Do not use a Pen application (INPUT_PROP_DIRECT) or a Puck tool (relative
 * mouse mapping).  Buttons live in the Pointer physical collection so Linux
 * maps them to BTN_LEFT/RIGHT/etc., on the SAME input device as the axes.
 */
static const uint8_t ABSOLUTE_MOUSE_REPORT_DESC[] = {
    0x05, 0x0D,       /* Usage Page (Digitizers) */
    0x09, 0x01,       /* Usage (Digitizer) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x20,       /* Usage (Stylus) */
    0xA1, 0x00,       /* Collection (Physical) */
    0x09, 0x42,       /* Usage (Tip Switch) */
    0x09, 0x32,       /* Usage (In Range) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x02,       /* Report Count (2) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x75, 0x06,       /* Report Size (6) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x01,       /* Input (Constant): padding */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x01,       /* Usage (Pointer) */
    0xA1, 0x00,       /* Collection (Physical) */
    0x05, 0x09,       /* Usage Page (Buttons) */
    0x19, 0x01,       /* Usage Minimum (1) */
    0x29, 0x05,       /* Usage Maximum (5) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x05,       /* Report Count (5) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute): buttons */
    0x75, 0x03,       /* Report Size (3) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x01,       /* Input (Constant): padding */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x30,       /* Usage (X) */
    0x09, 0x31,       /* Usage (Y) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xFF, 0x7F, /* Logical Maximum (32767) */
    0x75, 0x10,       /* Report Size (16) */
    0x95, 0x02,       /* Report Count (2) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute): X/Y */
    0x09, 0x38,       /* Usage (Wheel) */
    0x15, 0x81,       /* Logical Minimum (-127) */
    0x25, 0x7F,       /* Logical Maximum (127) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x06,       /* Input (Data, Variable, Relative): wheel */
    0x05, 0x0C,       /* Usage Page (Consumer) */
    0x0A, 0x38, 0x02, /* Usage (AC Pan) */
    0x81, 0x06,       /* Input (Data, Variable, Relative): pan */
    0xC0, 0xC0, 0xC0  /* End Pointer, Stylus, Application */
};

static int32_t clamp_position(int32_t value, int32_t size)
{
    if (value < 0) {
        return 0;
    }
    if (value >= size) {
        return size - 1;
    }
    return value;
}

static uint16_t absolute_axis(int32_t value, int32_t size)
{
    if (size <= 1) {
        return 0;
    }
    return (uint16_t)((int64_t)clamp_position(value, size) * HID_ABSOLUTE_MAX /
                      (size - 1));
}

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
static bool send_keyboard_report_with_shortcut(struct otg_bridge *b,
                                               uint8_t mods, int usage)
{
    if (!b->transport_ops->ready(b->transport_ctx)) {
        return false;
    }
    uint8_t report[AOA_KEYBOARD_REPORT_SIZE];
    memset(report, 0, sizeof(report));

    report[0] = b->hid_mods | mods;
    report[1] = 0; /* reserved */

    int n = 0;
    bool phantom = false;
    for (size_t i = 0; i < sizeof(b->keys); ++i) {
        if (b->keys[i] || (int)i == usage) {
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

    if (!b->transport_ops->send(b->transport_ctx, AOA_HID_ID_KEYBOARD, report,
                                sizeof(report))) {
        LOG_WARN("failed to send keyboard report\n");
        return false;
    }
    return true;
}

static void send_keyboard_report(struct otg_bridge *b)
{
    (void)send_keyboard_report_with_shortcut(b, 0, -1);
}

static uint64_t bridge_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
#endif
}

static void send_key_tap(struct otg_bridge *b, int usage)
{
    if (send_keyboard_report_with_shortcut(b, 0, usage))
        (void)send_keyboard_report_with_shortcut(b, 0, -1);
}

static int pin_digit_usage(char digit)
{
    if (digit >= '1' && digit <= '9') return HID_KEY_1 + (digit - '1');
    if (digit == '0') return HID_KEY_0;
    return -1;
}

static void schedule_unlock(struct otg_bridge *b)
{
    b->unlock_pending = true;
    b->unlock_due_ms = bridge_now_ms() + UNLOCK_WAKE_DELAY_MS;
    b->unlock_phase = UNLOCK_PHASE_WAIT_SWIPE;
    b->unlock_swipe_step = 0;
    /* Enter is widely accepted as a harmless wake-up key on Android lock
     * screens.  The swipe and PIN are delayed for the screen animation. */
    send_key_tap(b, HID_KEY_ENTER);
    LOG_DBG("unlock swipe scheduled after screen entry\n");
}

/* One click produces one complete system shortcut, never a held/repeating
 * Enter/Backspace or a Meta modifier applied to later typing. Compose reports
 * without changing physical keyboard state, including a physically held Meta.
 * Release the shortcut key before synthetic Meta, even after a send failure. */
static void send_navigation_shortcut(struct otg_bridge *b, uint8_t usage)
{
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(b->keys); ++i) count += b->keys[i];
    if (b->keys[usage] || count >= HID_KEYBOARD_MAX_KEYS) {
        LOG_WARN("navigation shortcut skipped: key already held or keyboard full\n");
        return;
    }
    if (send_keyboard_report_with_shortcut(b, HID_MOD_LGUI, -1)) {
        (void)send_keyboard_report_with_shortcut(b, HID_MOD_LGUI, usage);
    }
    (void)send_keyboard_report_with_shortcut(b, HID_MOD_LGUI, -1);
    send_keyboard_report(b);
}

static void send_relative_mouse_report(struct otg_bridge *b, uint8_t buttons,
                                       int8_t dx, int8_t dy, int8_t vscroll,
                                       int8_t hscroll)
{
    if (!b->transport_ops->ready(b->transport_ctx)) {
        return;
    }
    uint8_t report[AOA_MOUSE_REPORT_SIZE];
    report[0] = buttons;
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;
    report[3] = (uint8_t)vscroll;
    report[4] = (uint8_t)hscroll;
    if (!b->transport_ops->send(b->transport_ctx, AOA_HID_ID_MOUSE, report,
                                sizeof(report))) {
        LOG_WARN("failed to send mouse report\n");
    }
}

/* Every absolute report is self-contained, including clicks and wheel events.
 * A lost move therefore cannot leave the next click at an old location. */
static void send_mouse_state(struct otg_bridge *b, int8_t vs, int8_t hs)
{
    if (!b->transport_ops->ready(b->transport_ctx)) {
        return;
    }
    if (b->mouse_mode == OTG_MOUSE_RELATIVE) {
        send_relative_mouse_report(b, b->mouse_buttons, 0, 0, vs, hs);
        return;
    }

    uint16_t x = absolute_axis(b->mouse_x, b->screen_w);
    uint16_t y = absolute_axis(b->mouse_y, b->screen_h);
    uint8_t buttons = b->active ? b->mouse_buttons : 0;
    /* Like Android's mouse, left/right/middle count as pointer down. */
    uint8_t flags = b->active ? (uint8_t)(0x02 | !!(buttons & 0x07)) : 0;
    uint8_t report[AOA_ABSOLUTE_MOUSE_REPORT_SIZE] = {
        flags, buttons, (uint8_t)x, (uint8_t)(x >> 8),
        (uint8_t)y, (uint8_t)(y >> 8), (uint8_t)vs, (uint8_t)hs,
    };
    if (!b->transport_ops->send(b->transport_ctx, AOA_HID_ID_MOUSE, report,
                                sizeof(report))) {
        LOG_WARN("failed to send absolute mouse report\n");
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
        send_relative_mouse_report(b, buttons, sx, sy, 0, 0);
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
 * Best-effort relative-mode warp to (tx, ty) -- the equivalent of the
 * XWarpPointer() the real deskflow client does on enter (deskflow's
 * Client::enter() calls Screen::mouseMove() to place the local cursor on the
 * enter coordinates before any delta is applied).
 *
 * This compatibility path assumes unaccelerated 1:1 motion and matching
 * display bounds. Android mouse speed/acceleration can violate both legs;
 * absolute mode avoids this approximation entirely. For a relative HID:
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
        send_mouse_state(b, 0, 0);
    }
}

int otg_bridge_open(struct otg_bridge *b, const char *serial,
                    int32_t screen_w, int32_t screen_h,
                    enum otg_mouse_mode mouse_mode)
{
    memset(b, 0, sizeof(*b));
    if (screen_w < 1 || screen_w > INT16_MAX ||
        screen_h < 1 || screen_h > INT16_MAX) {
        LOG_ERR("screen width and height must be in 1..32767\n");
        return -1;
    }
    b->screen_w = screen_w;
    b->screen_h = screen_h;
    b->mouse_mode = mouse_mode;

    if (!aoa_hid_init(&b->aoa)) {
        return -1;
    }
    if (!aoa_hid_open(&b->aoa, serial)) {
        aoa_hid_destroy(&b->aoa);
        return -1;
    }
    b->transport_ops = &USB_TRANSPORT;
    b->transport_ctx = &b->aoa;
    b->external_transport = false;
    if (!b->transport_ops->register_report(
            b->transport_ctx, AOA_HID_ID_KEYBOARD, KEYBOARD_REPORT_DESC_FULL,
            sizeof(KEYBOARD_REPORT_DESC_FULL))) {
        LOG_ERR("could not register USB keyboard\n");
        otg_bridge_close(b);
        return -1;
    }
    const uint8_t *mouse_desc = mouse_mode == OTG_MOUSE_ABSOLUTE
                               ? ABSOLUTE_MOUSE_REPORT_DESC : MOUSE_REPORT_DESC;
    uint16_t mouse_desc_size = mouse_mode == OTG_MOUSE_ABSOLUTE
                              ? sizeof(ABSOLUTE_MOUSE_REPORT_DESC)
                              : sizeof(MOUSE_REPORT_DESC);
    if (!b->transport_ops->register_report(b->transport_ctx,
                                           AOA_HID_ID_MOUSE, mouse_desc,
                                           mouse_desc_size)) {
        LOG_ERR("could not register USB pointer\n");
        otg_bridge_close(b);
        return -1;
    }
    return 0;
}

int otg_bridge_open_transport(struct otg_bridge *b,
                              const struct otg_hid_transport_ops *ops,
                              void *ctx, int32_t screen_w, int32_t screen_h,
                              enum otg_mouse_mode mouse_mode)
{
    memset(b, 0, sizeof(*b));
    if (!ops || !ops->register_report || !ops->send || !ops->poll ||
        !ops->ready || !ops->close || screen_w < 1 || screen_w > INT16_MAX ||
        screen_h < 1 || screen_h > INT16_MAX) {
        return -1;
    }
    b->screen_w = screen_w;
    b->screen_h = screen_h;
    b->mouse_mode = mouse_mode;
    b->transport_ops = ops;
    b->transport_ctx = ctx;
    b->external_transport = true;
    const uint8_t *mouse_desc = mouse_mode == OTG_MOUSE_ABSOLUTE
                                    ? ABSOLUTE_MOUSE_REPORT_DESC
                                    : MOUSE_REPORT_DESC;
    uint16_t mouse_desc_size = mouse_mode == OTG_MOUSE_ABSOLUTE
                                   ? sizeof(ABSOLUTE_MOUSE_REPORT_DESC)
                                   : sizeof(MOUSE_REPORT_DESC);
    if (!ops->register_report(ctx, AOA_HID_ID_KEYBOARD,
                              KEYBOARD_REPORT_DESC_FULL,
                              sizeof(KEYBOARD_REPORT_DESC_FULL)) ||
        !ops->register_report(ctx, AOA_HID_ID_MOUSE, mouse_desc,
                              mouse_desc_size)) {
        ops->close(ctx);
        b->transport_ops = NULL;
        b->transport_ctx = NULL;
        return -1;
    }
    return 0;
}

void otg_bridge_close(struct otg_bridge *b)
{
    if (b->active) {
        otg_bridge_leave(b);
    }
    if (b->transport_ops) {
        b->transport_ops->close(b->transport_ctx);
        b->transport_ops = NULL;
        b->transport_ctx = NULL;
    }
    if (b->external_transport) {
        memset(&b->aoa, 0, sizeof(b->aoa));
    }
}

void otg_bridge_set_middle_button_action(struct otg_bridge *b,
                                         enum otg_middle_button_action action)
{
    if (action != OTG_MIDDLE_BUTTON_HOME) {
        action = OTG_MIDDLE_BUTTON_NORMAL;
    }
    if (b->middle_button_home_down && action != OTG_MIDDLE_BUTTON_HOME) {
        b->middle_button_home_down = false;
    }
    b->middle_button_action = action;
}

void otg_bridge_set_right_button_action(struct otg_bridge *b,
                                       enum otg_right_button_action action)
{
    if (action != OTG_RIGHT_BUTTON_BACK) {
        action = OTG_RIGHT_BUTTON_NORMAL;
    }
    if (b->right_button_back_down && action != OTG_RIGHT_BUTTON_BACK) {
        b->right_button_back_down = false;
    }
    b->right_button_action = action;
}

bool otg_bridge_check(struct otg_bridge *b)
{
    return b->transport_ops && b->transport_ops->poll(b->transport_ctx);
}

bool otg_bridge_ready(const struct otg_bridge *b)
{
    return b->transport_ops && b->transport_ops->ready(b->transport_ctx);
}

void otg_bridge_reset_input(struct otg_bridge *b)
{
    b->hid_mods = 0;
    memset(b->keys, 0, sizeof(b->keys));
    b->middle_button_home_down = false;
    b->right_button_back_down = false;
    b->mouse_buttons = 0;
    b->mouse_x = b->mouse_y = 0;
    b->active = b->have_mouse_pos = false;
    b->residual_hscroll = b->residual_vscroll = 0;
    b->unlock_attempted = false;
    b->unlock_pending = false;
    b->unlock_phase = UNLOCK_PHASE_NONE;
    b->unlock_swipe_step = 0;
    if (b->transport_ops && otg_bridge_ready(b)) {
        send_keyboard_report(b);
        send_mouse_state(b, 0, 0);
    }
}

bool otg_bridge_resize(struct otg_bridge *b, int32_t width, int32_t height)
{
    if (width < 1 || width > INT16_MAX || height < 1 || height > INT16_MAX) return false;
    if (width == b->screen_w && height == b->screen_h) return true;
    b->mouse_buttons = 0;
    b->residual_hscroll = b->residual_vscroll = 0;
    if (b->active) send_mouse_state(b, 0, 0); /* Release at the old drag location. */
    b->mouse_x = b->screen_w > 1 ? (int32_t)((int64_t)b->mouse_x * (width - 1) / (b->screen_w - 1)) : 0;
    b->mouse_y = b->screen_h > 1 ? (int32_t)((int64_t)b->mouse_y * (height - 1) / (b->screen_h - 1)) : 0;
    b->screen_w = width; b->screen_h = height;
    if (b->mouse_mode == OTG_MOUSE_RELATIVE) b->have_mouse_pos = false;
    if (b->active) send_mouse_state(b, 0, 0);
    return true;
}

void otg_bridge_enter(struct otg_bridge *b, int16_t x, int16_t y, uint16_t mask)
{
    /* Seed modifier state from the server's reported active-modifier mask;
     * subsequent modifier key events will refine it. */
    b->hid_mods = modmask_to_hid(mask);

    b->mouse_x = clamp_position(x, b->screen_w);
    b->mouse_y = clamp_position(y, b->screen_h);
    b->have_mouse_pos = true;
    b->active = true;
    b->mouse_buttons = 0;
    if (b->mouse_mode == OTG_MOUSE_ABSOLUTE) {
        send_mouse_state(b, 0, 0); /* hover at enter position, never click */
    } else {
        mouse_warp(b, b->mouse_x, b->mouse_y);
    }
    send_keyboard_report(b);
    if (b->unlock_pin[0] && !b->unlock_attempted) {
        b->unlock_attempted = true;
        if (!b->unlock_adb_gated) schedule_unlock(b);
        else LOG_DBG("waiting for ADB lock-state result\n");
    }
    LOG_DBG("enter at %d,%d mask=0x%04x\n", x, y, mask);
}

void otg_bridge_leave(struct otg_bridge *b)
{
    if (b->middle_button_home_down || b->right_button_back_down) {
        b->middle_button_home_down = b->right_button_back_down = false;
    }
    if (b->mouse_buttons) {
        /* Never leave a button stuck down on the device. */
        b->mouse_buttons = 0;
        send_mouse_state(b, 0, 0);
    }
    bool was_active = b->active;
    b->active = false;
    if (was_active && b->mouse_mode == OTG_MOUSE_ABSOLUTE) {
        send_mouse_state(b, 0, 0); /* end hover after releasing buttons */
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
    b->residual_hscroll = 0;
    b->residual_vscroll = 0;
    b->unlock_pending = false;
    b->unlock_phase = UNLOCK_PHASE_NONE;
    if (b->unlock_adb_gated) b->unlock_attempted = false;
}

bool otg_bridge_set_unlock_pin(struct otg_bridge *b, const char *pin)
{
    if (!b) return false;
    memset(b->unlock_pin, 0, sizeof(b->unlock_pin));
    b->unlock_attempted = false;
    b->unlock_pending = false;
    b->unlock_adb_gated = false;
    b->unlock_phase = UNLOCK_PHASE_NONE;
    b->unlock_swipe_step = 0;
    if (!pin || !*pin) return true;
    size_t length = strlen(pin);
    if (length < 4 || length > 32) return false;
    for (size_t i = 0; i < length; ++i) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    memcpy(b->unlock_pin, pin, length);
    return true;
}

void otg_bridge_set_unlock_adb_gated(struct otg_bridge *b, bool gated)
{
    if (!b) return;
    b->unlock_adb_gated = gated;
}

void otg_bridge_unlock_adb_state(struct otg_bridge *b, bool valid, bool locked)
{
    if (!b || !b->unlock_adb_gated || !b->active || !b->unlock_attempted ||
        b->unlock_pending || !valid || !locked) return;
    schedule_unlock(b);
}

void otg_bridge_unlock_tick(struct otg_bridge *b)
{
    if (!b || !b->unlock_pending || !b->active) return;
    if (bridge_now_ms() < b->unlock_due_ms) return;

    const uint64_t now = bridge_now_ms();
    if (b->unlock_phase == UNLOCK_PHASE_WAIT_SWIPE) {
        if (b->mouse_mode != OTG_MOUSE_ABSOLUTE) {
            b->unlock_pending = false;
            b->unlock_phase = UNLOCK_PHASE_NONE;
            LOG_WARN("automatic unlock requires absolute mouse mode; skipped\n");
            return;
        }
        b->unlock_saved_x = b->mouse_x;
        b->unlock_saved_y = b->mouse_y;
        b->mouse_x = b->screen_w / 2;
        b->mouse_y = b->screen_h - 1;
        b->mouse_buttons = 1; /* Tip Switch: begin the upward swipe. */
        send_mouse_state(b, 0, 0);
        b->unlock_swipe_step = 0;
        b->unlock_phase = UNLOCK_PHASE_SWIPE;
        b->unlock_due_ms = now + UNLOCK_SWIPE_INTERVAL_MS;
        return;
    }
    if (b->unlock_phase == UNLOCK_PHASE_SWIPE) {
        ++b->unlock_swipe_step;
        b->mouse_y = (int32_t)((int64_t)(b->screen_h - 1) *
                                (UNLOCK_SWIPE_STEPS - b->unlock_swipe_step) /
                                UNLOCK_SWIPE_STEPS);
        send_mouse_state(b, 0, 0);
        if (b->unlock_swipe_step < UNLOCK_SWIPE_STEPS) {
            b->unlock_due_ms = now + UNLOCK_SWIPE_INTERVAL_MS;
            return;
        }
        b->mouse_buttons = 0; /* Release at the top of the lock screen. */
        send_mouse_state(b, 0, 0);
        b->mouse_x = b->unlock_saved_x;
        b->mouse_y = b->unlock_saved_y;
        send_mouse_state(b, 0, 0);
        b->unlock_phase = UNLOCK_PHASE_WAIT_PIN;
        b->unlock_due_ms = now + UNLOCK_PIN_DELAY_MS;
        return;
    }
    if (b->unlock_phase == UNLOCK_PHASE_WAIT_PIN) {
        for (size_t i = 0; b->unlock_pin[i]; ++i) {
            const int usage = pin_digit_usage(b->unlock_pin[i]);
            if (usage >= 0) send_key_tap(b, usage);
        }
        send_key_tap(b, HID_KEY_ENTER);
        b->unlock_pending = false;
        b->unlock_phase = UNLOCK_PHASE_NONE;
        LOG_DBG("unlock PIN sequence sent\n");
    }
}

void otg_bridge_release_keyboard(struct otg_bridge *b)
{
    b->hid_mods = 0;
    memset(b->keys, 0, sizeof(b->keys));
    b->middle_button_home_down = false;
    b->right_button_back_down = false;
    send_keyboard_report(b);
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
    } else if (usage < (int)sizeof(b->keys)) {
        b->keys[usage] = down;
    } else {
        return; /* out of report range */
    }

    send_keyboard_report(b);
}

void otg_bridge_mouse_button(struct otg_bridge *b, uint8_t button, bool press)
{
    if (!b->active) {
        return;
    }
    if (button == 2 && b->middle_button_action == OTG_MIDDLE_BUTTON_HOME) {
        if (b->middle_button_home_down != press) {
            b->middle_button_home_down = press;
            if (press) send_navigation_shortcut(b, HID_KEY_ENTER);
        }
        return;
    }
    if (button == 3 && b->right_button_action == OTG_RIGHT_BUTTON_BACK) {
        if (b->right_button_back_down != press) {
            b->right_button_back_down = press;
            if (press) send_navigation_shortcut(b, HID_KEY_BACKSPACE);
        }
        return;
    }
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
    send_mouse_state(b, 0, 0);
}

void otg_bridge_mouse_move(struct otg_bridge *b, int16_t x, int16_t y)
{
    if (!b->active) {
        return;
    }
    int32_t tx = clamp_position(x, b->screen_w);
    int32_t ty = clamp_position(y, b->screen_h);
    int32_t dx = tx - b->mouse_x;
    int32_t dy = ty - b->mouse_y;
    bool had_position = b->have_mouse_pos;
    b->mouse_x = tx;
    b->mouse_y = ty;
    b->have_mouse_pos = true;

    if (b->mouse_mode == OTG_MOUSE_ABSOLUTE) {
        send_mouse_state(b, 0, 0);
    } else if (!had_position) {
        mouse_warp(b, tx, ty);
    } else {
        send_mouse_move(b, dx, dy);
    }
}

void otg_bridge_mouse_rel_move(struct otg_bridge *b, int16_t dx, int16_t dy)
{
    if (!b->active) {
        return;
    }
    /* Locked-screen DMRM has no absolute target. Accumulate within the screen
     * so reversing after an overshoot responds immediately. The next DMMV
     * provides the authoritative position again. */
    b->mouse_x = clamp_position(b->mouse_x + dx, b->screen_w);
    b->mouse_y = clamp_position(b->mouse_y + dy, b->screen_h);
    if (b->mouse_mode == OTG_MOUSE_ABSOLUTE) {
        send_mouse_state(b, 0, 0);
    } else {
        send_mouse_move(b, dx, dy);
    }
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
    send_mouse_state(b, vs, hs);
}
