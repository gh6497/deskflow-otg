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
bool aoa_hid_init(struct aoa_hid *a){memset(a,0,sizeof(*a));return true;}
bool aoa_hid_open(struct aoa_hid *a,const char *serial){(void)a;CHECK(!strcmp(serial,"mock"));return true;}
void aoa_hid_destroy(struct aoa_hid *a){(void)a;closes++;}
bool aoa_hid_register(struct aoa_hid *a,uint16_t id,const uint8_t *desc,uint16_t size)
{(void)a;CHECK(id==++registrations);CHECK(size>6 && desc[0]==5);return true;}
bool aoa_hid_send(struct aoa_hid *a,uint16_t id,const uint8_t *data,uint16_t size)
{(void)a;CHECK(size<=sizeof(report));last_id=id;last_size=size;memcpy(report,data,size);reports++;return true;}
bool aoa_hid_poll(struct aoa_hid *a){return !a->disconnected;}
bool aoa_hid_is_disconnected(const struct aoa_hid *a){return a->disconnected;}

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
    registrations = 0;
    CHECK(!otg_bridge_open(&b,"mock",1080,2340,OTG_MOUSE_ABSOLUTE));
    otg_bridge_set_middle_button_action(&b, OTG_MIDDLE_BUTTON_HOME);
    otg_bridge_enter(&b,0,0,0);
    otg_bridge_mouse_button(&b,2,true);
    CHECK(last_id==1 && report[2]==HID_KEY_F13);
    otg_bridge_mouse_button(&b,2,false);
    CHECK(last_id==1 && report[2]==0);
    otg_bridge_close(&b);
    puts("PASS: default USB descriptors, absolute coordinates, buttons, wheel, keyboard, Home shortcut and close lifecycle");
    return 0;
}
