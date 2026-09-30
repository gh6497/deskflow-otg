/*
 * bridge.h - glue between the Deskflow event stream and the AOA HID device.
 *
 * Maintains the current keyboard / mouse state and translates Deskflow events
 * into USB HID input reports.  The keyboard and relative mouse follow scrcpy;
 * the absolute pointer uses an HID tablet to avoid Android mouse acceleration.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_BRIDGE_H
#define DESKFLOW_OTG_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "aoa_hid.h"

enum otg_mouse_mode {
    OTG_MOUSE_ABSOLUTE,
    OTG_MOUSE_RELATIVE,
};

struct otg_bridge {
    struct aoa_hid aoa;

    /* keyboard state */
    uint8_t hid_mods;         /* HID modifier byte (bits 0..7) */
    bool keys[0x66];          /* pressed non-modifier HID usages (0x00..0x65) */

    /* mouse state */
    enum otg_mouse_mode mouse_mode;
    uint8_t mouse_buttons;    /* HID button byte */
    int32_t mouse_x, mouse_y; /* last absolute cursor position */
    bool have_mouse_pos;
    /*
     * True between "enter" and "leave", i.e. while the phone is the screen
     * the server is sending mouse events for.  Motion received outside that
     * window is dropped instead of being replayed to the device.
     */
    bool active;
    /* Virtual screen size reported to the server.  Absolute mode maps this
     * rectangle to the tablet's full logical range, independent of Android's
     * display resolution and mouse speed. */
    int32_t screen_w, screen_h;
    float residual_hscroll;
    float residual_vscroll;
};

/*
 * Open the Android device over USB and register the HID keyboard + mouse.
 *
 * `screen_w`/`screen_h` must be in 1..32767 (Deskflow signed coordinates).
 * Absolute mode scales these coordinates to the full Android display.
 * Relative compatibility mode additionally assumes matching display bounds
 * and unaccelerated 1:1 motion, which Android does not normally guarantee.
 * Returns 0 on success, non-zero on failure.
 */
int otg_bridge_open(struct otg_bridge *b, const char *serial,
                    int32_t screen_w, int32_t screen_h,
                    enum otg_mouse_mode mouse_mode);

void otg_bridge_close(struct otg_bridge *b);

/* Event handlers (driven by the deskflow client callbacks). */
void otg_bridge_enter(struct otg_bridge *b, int16_t x, int16_t y, uint16_t mask);
void otg_bridge_leave(struct otg_bridge *b);
void otg_bridge_key(struct otg_bridge *b, uint32_t keyid, bool down);
void otg_bridge_mouse_button(struct otg_bridge *b, uint8_t button, bool press);
void otg_bridge_mouse_move(struct otg_bridge *b, int16_t x, int16_t y);
void otg_bridge_mouse_rel_move(struct otg_bridge *b, int16_t dx, int16_t dy);
void otg_bridge_mouse_wheel(struct otg_bridge *b, int16_t x, int16_t y);

/*
 * Periodic health check: returns true if the USB device is still connected.
 * Called from the event-loop idle callback so a phone unplug is noticed and
 * the connection to the deskflow server can be torn down (which lets the
 * server return the cursor to the primary screen).
 */
bool otg_bridge_check(struct otg_bridge *b);

#endif /* DESKFLOW_OTG_BRIDGE_H */
