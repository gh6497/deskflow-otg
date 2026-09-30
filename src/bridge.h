/*
 * bridge.h - glue between the Deskflow event stream and the AOA HID device.
 *
 * Maintains the current keyboard / mouse state and translates Deskflow events
 * into USB HID input reports, exactly like scrcpy's `keyboard_aoa` /
 * `mouse_aoa` + `hid_keyboard` / `hid_mouse` do internally.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_BRIDGE_H
#define DESKFLOW_OTG_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "aoa_hid.h"

struct otg_bridge {
    struct aoa_hid aoa;

    /* keyboard state */
    uint8_t hid_mods;         /* HID modifier byte (bits 0..7) */
    bool keys[0x66];          /* pressed non-modifier HID usages (0x00..0x65) */

    /* mouse state */
    uint8_t mouse_buttons;    /* HID button byte */
    int32_t mouse_x, mouse_y; /* last absolute cursor position */
    bool have_mouse_pos;
    float residual_hscroll;
    float residual_vscroll;
};

/*
 * Open the Android device over USB and register the HID keyboard + mouse.
 * Returns 0 on success, non-zero on failure.
 */
int otg_bridge_open(struct otg_bridge *b, const char *serial);

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
