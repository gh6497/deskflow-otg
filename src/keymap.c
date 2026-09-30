/*
 * keymap.c - see keymap.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "keymap.h"

/*
 * deskflow KeyID constants, mirroring deskflow/src/lib/deskflow/KeyTypes.h.
 * Only the subset we translate is listed.
 */
#define KF_KEY_BACKSPACE 0xEF08
#define KF_KEY_TAB       0xEF09
#define KF_KEY_RETURN    0xEF0D
#define KF_KEY_PAUSE     0xEF13
#define KF_KEY_SCROLLLOCK 0xEF14
#define KF_KEY_ESCAPE    0xEF1B
#define KF_KEY_DELETE    0xEFFF

#define KF_KEY_HOME      0xEF50
#define KF_KEY_LEFT      0xEF51
#define KF_KEY_UP        0xEF52
#define KF_KEY_RIGHT     0xEF53
#define KF_KEY_DOWN      0xEF54
#define KF_KEY_PAGEUP    0xEF55
#define KF_KEY_PAGEDOWN  0xEF56
#define KF_KEY_END       0xEF57
#define KF_KEY_INSERT    0xEF63

#define KF_KEY_ALTGR     0xEF7E
#define KF_KEY_NUMLOCK   0xEF7F

#define KF_KEY_KP_SPACE     0xEF80
#define KF_KEY_KP_TAB       0xEF89
#define KF_KEY_KP_ENTER     0xEF8D
#define KF_KEY_KP_EQUAL     0xEFBD
#define KF_KEY_KP_MULTIPLY  0xEFAA
#define KF_KEY_KP_ADD       0xEFAB
#define KF_KEY_KP_SEPARATOR 0xEFAC
#define KF_KEY_KP_SUBTRACT  0xEFAD
#define KF_KEY_KP_DECIMAL   0xEFAE
#define KF_KEY_KP_DIVIDE    0xEFAF
#define KF_KEY_KP_0         0xEFB0
#define KF_KEY_KP_1         0xEFB1
#define KF_KEY_KP_2         0xEFB2
#define KF_KEY_KP_3         0xEFB3
#define KF_KEY_KP_4         0xEFB4
#define KF_KEY_KP_5         0xEFB5
#define KF_KEY_KP_6         0xEFB6
#define KF_KEY_KP_7         0xEFB7
#define KF_KEY_KP_8         0xEFB8
#define KF_KEY_KP_9         0xEFB9

#define KF_KEY_F1          0xEFBE
#define KF_KEY_F2          0xEFBF
#define KF_KEY_F3          0xEFC0
#define KF_KEY_F4          0xEFC1
#define KF_KEY_F5          0xEFC2
#define KF_KEY_F6          0xEFC3
#define KF_KEY_F7          0xEFC4
#define KF_KEY_F8          0xEFC5
#define KF_KEY_F9          0xEFC6
#define KF_KEY_F10         0xEFC7
#define KF_KEY_F11         0xEFC8
#define KF_KEY_F12         0xEFC9
#define KF_KEY_F13         0xEFCA
#define KF_KEY_F14         0xEFCB
#define KF_KEY_F15         0xEFCC
#define KF_KEY_F16         0xEFCD
#define KF_KEY_F17         0xEFCE
#define KF_KEY_F18         0xEFCF
#define KF_KEY_F19         0xEFD0
#define KF_KEY_F20         0xEFD1
#define KF_KEY_F21         0xEFD2
#define KF_KEY_F22         0xEFD3
#define KF_KEY_F23         0xEFD4
#define KF_KEY_F24         0xEFD5

#define KF_KEY_SHIFT_L     0xEFE1
#define KF_KEY_SHIFT_R     0xEFE2
#define KF_KEY_CONTROL_L   0xEFE3
#define KF_KEY_CONTROL_R   0xEFE4
#define KF_KEY_CAPSLOCK    0xEFE5
#define KF_KEY_META_L      0xEFE7
#define KF_KEY_META_R      0xEFE8
#define KF_KEY_ALT_L       0xEFE9
#define KF_KEY_ALT_R       0xEFEA
#define KF_KEY_SUPER_L     0xEFEB
#define KF_KEY_SUPER_R     0xEFEC

int keyid_to_hid(uint32_t keyid)
{
    /* Letters: 'a'..'z' and 'A'..'Z' -> HID 0x04..0x1D. */
    if (keyid >= 'a' && keyid <= 'z') {
        return HID_KEY_A + (int)(keyid - 'a');
    }
    if (keyid >= 'A' && keyid <= 'Z') {
        return HID_KEY_A + (int)(keyid - 'A');
    }

    /* Digits: '1'..'9' -> 0x1E..0x26, '0' -> 0x27. */
    if (keyid >= '1' && keyid <= '9') {
        return HID_KEY_1 + (int)(keyid - '1');
    }
    if (keyid == '0') {
        return HID_KEY_0;
    }

    /* US-layout punctuation.  Shifted symbols map to the same physical key
     * as their unshifted counterpart; the shift modifier is tracked
     * separately (from the explicit modifier key events / the enter mask). */
    switch (keyid) {
        case ' ':  return HID_KEY_SPACE;
        case '!':  return HID_KEY_1;         /* Shift+1 */
        case '"':  return 0x34;              /* Shift+' */
        case '#':  return 0x20;              /* Shift+3 */
        case '$':  return 0x21;              /* Shift+4 */
        case '%':  return 0x22;              /* Shift+5 */
        case '&':  return 0x23;              /* Shift+7 */
        case '\'': return 0x34;
        case '(':  return 0x26;              /* Shift+9 */
        case ')':  return HID_KEY_0;         /* Shift+0 */
        case '*':  return 0x25;              /* Shift+8 */
        case '+':  return 0x2E;              /* Shift+= */
        case ',':  return 0x36;
        case '-':  return 0x2D;
        case '.':  return 0x37;
        case '/':  return 0x38;
        case ':':  return 0x33;              /* Shift+; */
        case ';':  return 0x33;
        case '<':  return 0x36;              /* Shift+, */
        case '=':  return 0x2E;
        case '>':  return 0x37;              /* Shift+. */
        case '?':  return 0x38;              /* Shift+/ */
        case '@':  return 0x1F;              /* Shift+2 */
        case '[':  return 0x2F;
        case '\\': return 0x31;
        case ']':  return 0x30;
        case '^':  return 0x23;              /* Shift+6 */
        case '_':  return 0x2D;              /* Shift+- */
        case '`':  return 0x35;
        case '{':  return 0x2F;              /* Shift+[ */
        case '|':  return 0x31;              /* Shift+\ */
        case '}':  return 0x30;              /* Shift+] */
        case '~':  return 0x35;              /* Shift+` */
        default:   break;
    }

    /* Extended / control keys. */
    switch (keyid) {
        case KF_KEY_BACKSPACE:  return HID_KEY_BACKSPACE;
        case KF_KEY_TAB:        return HID_KEY_TAB;
        case KF_KEY_RETURN:     return HID_KEY_ENTER;
        case KF_KEY_PAUSE:      return HID_KEY_PAUSE;
        case KF_KEY_SCROLLLOCK: return HID_KEY_SCROLLLOCK;
        case KF_KEY_ESCAPE:     return HID_KEY_ESCAPE;
        case KF_KEY_DELETE:     return HID_KEY_DELETE;
        case KF_KEY_HOME:       return HID_KEY_HOME;
        case KF_KEY_LEFT:       return HID_KEY_LEFT;
        case KF_KEY_UP:         return HID_KEY_UP;
        case KF_KEY_RIGHT:      return HID_KEY_RIGHT;
        case KF_KEY_DOWN:       return HID_KEY_DOWN;
        case KF_KEY_PAGEUP:     return HID_KEY_PAGEUP;
        case KF_KEY_PAGEDOWN:   return HID_KEY_PAGEDOWN;
        case KF_KEY_END:        return HID_KEY_END;
        case KF_KEY_INSERT:     return HID_KEY_INSERT;
        case KF_KEY_NUMLOCK:    return HID_KEY_NUMLOCK;
        case KF_KEY_ALTGR:      return HID_KEY_RALT;

        case KF_KEY_KP_SPACE:     return HID_KEY_SPACE;
        case KF_KEY_KP_TAB:       return HID_KEY_TAB;
        case KF_KEY_KP_ENTER:     return HID_KEY_KP_ENTER;
        case KF_KEY_KP_EQUAL:     return 0x2E; /* = */
        case KF_KEY_KP_MULTIPLY:  return HID_KEY_KP_MULTIPLY;
        case KF_KEY_KP_ADD:       return HID_KEY_KP_PLUS;
        case KF_KEY_KP_SUBTRACT:  return HID_KEY_KP_MINUS;
        case KF_KEY_KP_DECIMAL:   return HID_KEY_KP_PERIOD;
        case KF_KEY_KP_DIVIDE:    return HID_KEY_KP_DIVIDE;
        case KF_KEY_KP_SEPARATOR: return HID_KEY_KP_PERIOD;
        case KF_KEY_KP_0: return HID_KEY_KP_0;
        case KF_KEY_KP_1: return HID_KEY_KP_1;
        case KF_KEY_KP_2: return HID_KEY_KP_2;
        case KF_KEY_KP_3: return HID_KEY_KP_3;
        case KF_KEY_KP_4: return HID_KEY_KP_4;
        case KF_KEY_KP_5: return HID_KEY_KP_5;
        case KF_KEY_KP_6: return HID_KEY_KP_6;
        case KF_KEY_KP_7: return HID_KEY_KP_7;
        case KF_KEY_KP_8: return HID_KEY_KP_8;
        case KF_KEY_KP_9: return HID_KEY_KP_9;

        case KF_KEY_F1:  return HID_KEY_F1;
        case KF_KEY_F2:  return HID_KEY_F1 + 1;
        case KF_KEY_F3:  return HID_KEY_F1 + 2;
        case KF_KEY_F4:  return HID_KEY_F1 + 3;
        case KF_KEY_F5:  return HID_KEY_F1 + 4;
        case KF_KEY_F6:  return HID_KEY_F1 + 5;
        case KF_KEY_F7:  return HID_KEY_F1 + 6;
        case KF_KEY_F8:  return HID_KEY_F1 + 7;
        case KF_KEY_F9:  return HID_KEY_F1 + 8;
        case KF_KEY_F10: return HID_KEY_F1 + 9;
        case KF_KEY_F11: return HID_KEY_F1 + 10;
        case KF_KEY_F12: return HID_KEY_F1 + 11;
        case KF_KEY_F13: return HID_KEY_F13;
        case KF_KEY_F14: return HID_KEY_F13 + 1;
        case KF_KEY_F15: return HID_KEY_F13 + 2;
        case KF_KEY_F16: return HID_KEY_F13 + 3;
        case KF_KEY_F17: return HID_KEY_F13 + 4;
        case KF_KEY_F18: return HID_KEY_F13 + 5;
        case KF_KEY_F19: return HID_KEY_F13 + 6;
        case KF_KEY_F20: return HID_KEY_F13 + 7;
        case KF_KEY_F21: return HID_KEY_F13 + 8;
        case KF_KEY_F22: return HID_KEY_F13 + 9;
        case KF_KEY_F23: return HID_KEY_F13 + 10;
        case KF_KEY_F24: return HID_KEY_F13 + 11;

        case KF_KEY_SHIFT_L:   return HID_KEY_LSHIFT;
        case KF_KEY_SHIFT_R:   return HID_KEY_RSHIFT;
        case KF_KEY_CONTROL_L: return HID_KEY_LCTRL;
        case KF_KEY_CONTROL_R: return HID_KEY_RCTRL;
        case KF_KEY_CAPSLOCK:  return HID_KEY_CAPSLOCK;
        case KF_KEY_META_L:    return HID_KEY_LGUI;
        case KF_KEY_META_R:    return HID_KEY_RGUI;
        case KF_KEY_ALT_L:     return HID_KEY_LALT;
        case KF_KEY_ALT_R:     return HID_KEY_RALT;
        case KF_KEY_SUPER_L:   return HID_KEY_LGUI;
        case KF_KEY_SUPER_R:   return HID_KEY_RGUI;
        default: break;
    }

    return -1;
}

uint8_t modmask_to_hid(uint16_t mask)
{
    uint8_t mods = 0;
    if (mask & DF_MOD_SHIFT)   mods |= HID_MOD_LSHIFT;
    if (mask & DF_MOD_CONTROL) mods |= HID_MOD_LCTRL;
    if (mask & DF_MOD_ALT)     mods |= HID_MOD_LALT;
    if (mask & DF_MOD_META)    mods |= HID_MOD_LGUI;
    if (mask & DF_MOD_SUPER)   mods |= HID_MOD_LGUI;
    if (mask & DF_MOD_ALTGR)   mods |= HID_MOD_RALT;
    return mods;
}

bool hid_usage_is_modifier(uint8_t usage, uint8_t *mod_bit_out)
{
    uint8_t bit;
    switch (usage) {
        case HID_KEY_LCTRL:  bit = HID_MOD_LCTRL;  break;
        case HID_KEY_LSHIFT: bit = HID_MOD_LSHIFT; break;
        case HID_KEY_LALT:   bit = HID_MOD_LALT;   break;
        case HID_KEY_LGUI:   bit = HID_MOD_LGUI;   break;
        case HID_KEY_RCTRL:  bit = HID_MOD_RCTRL;  break;
        case HID_KEY_RSHIFT: bit = HID_MOD_RSHIFT; break;
        case HID_KEY_RALT:   bit = HID_MOD_RALT;   break;
        case HID_KEY_RGUI:   bit = HID_MOD_RGUI;   break;
        default: return false;
    }
    if (mod_bit_out) {
        *mod_bit_out = bit;
    }
    return true;
}
