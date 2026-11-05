/* USB wire regression using an in-memory AOA device; no libusb calls.
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge.h"
#include "keymap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static uint16_t last_id,last_size;
static uint8_t report[8];
static int registrations,reports,closes;
static uint8_t keyboard_reports[256][8];
static unsigned keyboard_count;
static int fail_keyboard_report = -1;
bool aoa_hid_init(struct aoa_hid *a){memset(a,0,sizeof(*a));return true;}
bool aoa_hid_open(struct aoa_hid *a,const char *serial){(void)a;CHECK(!strcmp(serial,"mock"));return true;}
void aoa_hid_destroy(struct aoa_hid *a){(void)a;closes++;}
bool aoa_hid_register(struct aoa_hid *a,uint16_t id,const uint8_t *desc,uint16_t size)
{(void)a;CHECK(id==++registrations);CHECK(size>6 && desc[0]==5);return true;}
bool aoa_hid_send(struct aoa_hid *a,uint16_t id,const uint8_t *data,uint16_t size)
{
    (void)a;CHECK(size<=sizeof(report));last_id=id;last_size=size;
    memcpy(report,data,size);reports++;
    if (id == AOA_HID_ID_KEYBOARD) {
        CHECK(size == 8 && keyboard_count < 256);
        memcpy(keyboard_reports[keyboard_count++], data, size);
        if ((int)keyboard_count == fail_keyboard_report) return false;
    }
    return true;
}
bool aoa_hid_poll(struct aoa_hid *a){return !a->disconnected;}
bool aoa_hid_is_disconnected(const struct aoa_hid *a){return a->disconnected;}

static bool report_has_key(const uint8_t *data, uint8_t key)
{
    for (unsigned i = 2; i < sizeof(report); ++i) if (data[i] == key) return true;
    return false;
}

static void check_shortcut(struct otg_bridge *b, uint8_t button, uint8_t usage)
{
    unsigned before = keyboard_count;
    bool physical_keys[sizeof(b->keys)];
    memcpy(physical_keys, b->keys, sizeof(physical_keys));
    uint8_t physical_mods = b->hid_mods;
    otg_bridge_mouse_button(b, button, true);
    CHECK(keyboard_count == before + 4 && last_id == AOA_HID_ID_KEYBOARD);
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *data = keyboard_reports[before + i];
        CHECK(data[0] == (i == 3 ? physical_mods : (physical_mods | HID_MOD_LGUI)));
        CHECK(report_has_key(data, usage) == (i == 1));
        CHECK(!report_has_key(data, HID_KEY_F13) && !report_has_key(data, HID_KEY_F14));
        for (unsigned j = 1; j < sizeof(b->keys); ++j)
            if (physical_keys[j]) CHECK(report_has_key(data, (uint8_t)j));
    }
    CHECK(!memcmp(b->keys, physical_keys, sizeof(physical_keys)) && b->hid_mods == physical_mods);
    CHECK(!b->mouse_buttons);
}

static void test_navigation_shortcuts(enum otg_mouse_mode mode)
{
    struct otg_bridge b;
    registrations = 0;
    CHECK(!otg_bridge_open(&b, "mock", 1080, 2340, mode));
    CHECK(b.right_button_action == OTG_RIGHT_BUTTON_NORMAL);
    otg_bridge_set_right_button_action(&b, OTG_RIGHT_BUTTON_BACK);
    int before = reports;
    otg_bridge_mouse_button(&b, 3, true);
    CHECK(reports == before); /* Ignore input outside the phone. */
    otg_bridge_enter(&b, 0, 0, 0);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    before = reports;
    otg_bridge_mouse_button(&b, 3, true);
    CHECK(reports == before); /* Duplicate down does not repeat Back. */
    otg_bridge_key(&b, 'a', true);
    CHECK(report[0] == 0 && report_has_key(report, HID_KEY_A)); /* No Meta left held. */
    before = reports;
    otg_bridge_mouse_button(&b, 3, false);
    otg_bridge_mouse_button(&b, 3, false);
    CHECK(reports == before); /* Mouse release has no key side effects. */
    otg_bridge_set_middle_button_action(&b, OTG_MIDDLE_BUTTON_HOME);
    check_shortcut(&b, 2, HID_KEY_ENTER);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE); /* Both mouse buttons may stay held. */
    otg_bridge_leave(&b);
    CHECK(!b.middle_button_home_down && !b.right_button_back_down);
    before = reports;
    otg_bridge_mouse_button(&b, 3, false);
    CHECK(reports == before);
    otg_bridge_enter(&b, 0, 0, 0);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    otg_bridge_release_keyboard(&b);
    CHECK(last_id == 1 && !report[0] && !b.right_button_back_down);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    otg_bridge_reset_input(&b);
    CHECK(!b.right_button_back_down && !b.hid_mods && !b.active);
    CHECK(b.right_button_action == OTG_RIGHT_BUTTON_BACK);
    otg_bridge_enter(&b, 0, 0, 0);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    otg_bridge_set_right_button_action(&b, OTG_RIGHT_BUTTON_NORMAL);
    CHECK(last_id == 1 && !report[0] && !b.right_button_back_down);
    otg_bridge_mouse_button(&b, 3, true);
    CHECK(last_id == 2 && b.mouse_buttons == 2); /* Normal right click restored. */
    otg_bridge_mouse_button(&b, 3, false);
    CHECK(last_id == 2 && !b.mouse_buttons);
    otg_bridge_set_right_button_action(&b, (enum otg_right_button_action)99);
    CHECK(b.right_button_action == OTG_RIGHT_BUTTON_NORMAL);
    otg_bridge_set_right_button_action(&b, OTG_RIGHT_BUTTON_BACK);

    /* A physical Win/Ctrl and held letter survive all synthetic down/up phases. */
    otg_bridge_key(&b, 0xEFEB, true);
    otg_bridge_key(&b, 0xEFE3, true);
    otg_bridge_key(&b, 'a', true);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    otg_bridge_mouse_button(&b, 3, false);
    otg_bridge_key(&b, 0xEFEB, false);
    CHECK(report[0] == HID_MOD_LCTRL && b.hid_mods == HID_MOD_LCTRL);
    otg_bridge_key(&b, 0xEFE3, false);

    /* Never release/repress a physically held shortcut key or cause rollover. */
    otg_bridge_key(&b, 0xEF08, true);
    before = reports;
    otg_bridge_mouse_button(&b, 3, true);
    CHECK(reports == before && b.keys[HID_KEY_BACKSPACE]);
    otg_bridge_mouse_button(&b, 3, false);
    otg_bridge_key(&b, 0xEF08, false);
    for (uint32_t key = 'b'; key <= 'f'; ++key) otg_bridge_key(&b, key, true);
    before = reports;
    otg_bridge_mouse_button(&b, 3, true);
    CHECK(reports == before && b.keys[HID_KEY_A]);
    otg_bridge_mouse_button(&b, 3, false);
    otg_bridge_release_keyboard(&b);

    /* A transport failure during chord down still attempts key-up then Meta-up. */
    fail_keyboard_report = (int)keyboard_count + 2;
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    fail_keyboard_report = -1;
    otg_bridge_mouse_button(&b, 3, false);
    check_shortcut(&b, 3, HID_KEY_BACKSPACE);
    otg_bridge_close(&b);
    CHECK(!b.hid_mods && !b.right_button_back_down);
}

int main(void)
{
    struct otg_bridge b;
    CHECK(!otg_bridge_open(&b,"mock",1080,2340,OTG_MOUSE_ABSOLUTE));
    CHECK(registrations==2 && reports==0 && otg_bridge_ready(&b));
    otg_bridge_enter(&b,0,0,0);CHECK(reports==2 && last_id==1 && last_size==8);
    otg_bridge_mouse_move(&b,1079,2339);
    CHECK(last_id==2 && last_size==8 && report[0]==2 && report[1]==0);
    CHECK(report[2]==255 && report[3]==127 && report[4]==255 && report[5]==127);
    otg_bridge_mouse_button(&b,1,true);CHECK(report[0]==3 && report[1]==1);
    otg_bridge_mouse_button(&b,3,true);CHECK(report[1]==3);
    otg_bridge_mouse_wheel(&b,120,120);CHECK(report[6]==1 && report[7]==1);
    otg_bridge_key(&b,'a',true);CHECK(last_id==1 && report[0]==0 && report[2]==4);
    otg_bridge_key(&b,'a',false);CHECK(report[2]==0);
    otg_bridge_key(&b,'a',true);
    CHECK(otg_bridge_resize(&b,2340,1080));
    CHECK(b.active && b.keys[4] && !b.mouse_buttons && registrations==2 && closes==0);
    CHECK(b.mouse_x==2339 && b.mouse_y==1079 && b.screen_w==2340 && b.screen_h==1080);
    CHECK(last_id==2 && report[0]==2 && report[1]==0 && report[2]==255 && report[3]==127);
    otg_bridge_mouse_move(&b,0,0);CHECK(report[2]==0 && report[4]==0);
    otg_bridge_mouse_move(&b,2339,1079);CHECK(report[2]==255 && report[4]==255);
    int unchanged=reports;CHECK(otg_bridge_resize(&b,2340,1080) && reports==unchanged);
    CHECK(!otg_bridge_resize(&b,0,1080) && !otg_bridge_resize(&b,1080,32768) && reports==unchanged);
    CHECK(otg_bridge_resize(&b,1,1) && b.mouse_x==0 && b.mouse_y==0);
    CHECK(otg_bridge_resize(&b,1080,2340));
    otg_bridge_key(&b,'a',false);
    otg_bridge_leave(&b);CHECK(last_id==2 && report[0]==0 && report[1]==0);
    int before=reports;otg_bridge_mouse_move(&b,50,50);CHECK(reports==before);
    otg_bridge_close(&b);CHECK(closes==1);
    otg_bridge_close(&b);CHECK(closes==1);
    test_navigation_shortcuts(OTG_MOUSE_ABSOLUTE);
    test_navigation_shortcuts(OTG_MOUSE_RELATIVE);
    puts("PASS: default USB descriptors, absolute coordinates, buttons, wheel, keyboard, Home/Back shortcuts and close lifecycle");
    return 0;
}
