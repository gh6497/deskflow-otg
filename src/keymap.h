/*
 * keymap.h - mapping between the Deskflow wire protocol key identifiers and
 *            USB HID keyboard usages.
 *
 * This file is part of deskflow-otg, a Deskflow <-> scrcpy-OTG bridge.
 *
 * The mapping semantics below are a faithful re-implementation of the logic in
 * scrcpy's `app/src/hid/hid_keyboard.c` (the "AOAv2 HID keyboard" layer) and
 * of the key constants in deskflow's `src/lib/deskflow/KeyTypes.h`.
 *
 * Deskflow KeyID values (the "virtual key" sent on the wire):
 *   - Printable ASCII characters map to their ASCII/Unicode value
 *     (e.g. 'a' == 0x61, '!' == 0x21).
 *   - Extended/control keys live in 0xE000..0xEFFF and equal
 *     (X11 keysym - 0x1000), e.g. BackSpace == 0xEF08 (XK_BackSpace 0xFF08).
 *
 * USB HID keyboard usages (HID Usage Tables, page 0x07) are exactly the
 * values scrcpy calls "scancode": 0x04 == 'a', 0x28 == Return, 0xE0 == LCtrl,
 * etc.  Android interprets these directly, so this is the value we must put
 * into the HID input report.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_KEYMAP_H
#define DESKFLOW_OTG_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

/* USB HID keyboard usage IDs we emit (page 0x07, "Keyboard/Keypad"). */
#define HID_KEY_A            0x04
#define HID_KEY_Z            0x1D
#define HID_KEY_1            0x1E
#define HID_KEY_0            0x27
#define HID_KEY_ENTER        0x28
#define HID_KEY_ESCAPE       0x29
#define HID_KEY_BACKSPACE    0x2A
#define HID_KEY_TAB          0x2B
#define HID_KEY_SPACE        0x2C
#define HID_KEY_CAPSLOCK     0x39
#define HID_KEY_F1           0x3A
#define HID_KEY_F12          0x45
#define HID_KEY_PRINTSCREEN  0x46
#define HID_KEY_SCROLLLOCK   0x47
#define HID_KEY_PAUSE        0x48
#define HID_KEY_INSERT       0x49
#define HID_KEY_HOME         0x4A
#define HID_KEY_PAGEUP       0x4B
#define HID_KEY_DELETE       0x4C
#define HID_KEY_END          0x4D
#define HID_KEY_PAGEDOWN     0x4E
#define HID_KEY_RIGHT        0x4F
#define HID_KEY_LEFT         0x50
#define HID_KEY_DOWN         0x51
#define HID_KEY_UP           0x52
#define HID_KEY_NUMLOCK      0x53
#define HID_KEY_KP_DIVIDE    0x54
#define HID_KEY_KP_MULTIPLY  0x55
#define HID_KEY_KP_MINUS     0x56
#define HID_KEY_KP_PLUS      0x57
#define HID_KEY_KP_ENTER     0x58
#define HID_KEY_KP_1         0x59
#define HID_KEY_KP_2         0x5A
#define HID_KEY_KP_3         0x5B
#define HID_KEY_KP_4         0x5C
#define HID_KEY_KP_5         0x5D
#define HID_KEY_KP_6         0x5E
#define HID_KEY_KP_7         0x5F
#define HID_KEY_KP_8         0x60
#define HID_KEY_KP_9         0x61
#define HID_KEY_KP_0         0x62
#define HID_KEY_KP_PERIOD    0x63
#define HID_KEY_F13          0x68
#define HID_KEY_F24          0x73
#define HID_KEY_LCTRL        0xE0
#define HID_KEY_LSHIFT       0xE1
#define HID_KEY_LALT         0xE2
#define HID_KEY_LGUI         0xE3
#define HID_KEY_RCTRL        0xE4
#define HID_KEY_RSHIFT       0xE5
#define HID_KEY_RALT         0xE6
#define HID_KEY_RGUI         0xE7

/* Modifier byte bit positions (HID keyboard input report, byte 0). */
#define HID_MOD_LCTRL   (1u << 0)
#define HID_MOD_LSHIFT  (1u << 1)
#define HID_MOD_LALT    (1u << 2)
#define HID_MOD_LGUI    (1u << 3)
#define HID_MOD_RCTRL   (1u << 4)
#define HID_MOD_RSHIFT  (1u << 5)
#define HID_MOD_RALT    (1u << 6)
#define HID_MOD_RGUI    (1u << 7)

/* Deskflow modifier-mask bits (src/lib/deskflow/KeyTypes.h). */
#define DF_MOD_SHIFT    0x0001
#define DF_MOD_CONTROL  0x0002
#define DF_MOD_ALT      0x0004
#define DF_MOD_META     0x0008
#define DF_MOD_SUPER    0x0010
#define DF_MOD_ALTGR    0x0020

/*
 * Map a deskflow KeyID to a USB HID keyboard usage.
 *
 * Returns a value in 0x00..0xE7 on success, or -1 when the key is not
 * representable (unknown / dead key / group-change pseudo-key).
 */
int keyid_to_hid(uint32_t keyid);

/*
 * Map a deskflow side-agnostic modifier mask (as received in the "enter"
 * message) to a HID modifier byte.  Side-agnostic modifiers are assigned to
 * the left-hand HID modifiers, except AltGr which maps to right Alt.
 */
uint8_t modmask_to_hid(uint16_t mask);

/*
 * Translate a HID keyboard usage (0x00..0xE7) to its modifier-byte bit, or
 * return false if the usage is not a modifier key.
 */
bool hid_usage_is_modifier(uint8_t usage, uint8_t *mod_bit_out);

#endif /* DESKFLOW_OTG_KEYMAP_H */
