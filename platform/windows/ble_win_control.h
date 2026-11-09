/* GUI-owned Windows BLE session control; no Bluetooth or OS dependencies.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_WIN_CONTROL_H
#define OTG_BLE_WIN_CONTROL_H
#include <stdbool.h>
#include <stddef.h>

enum otg_ble_control_phase { OTG_BLE_ACTIVE, OTG_BLE_PAUSING, OTG_BLE_IDLE };
struct otg_ble_control {
    enum otg_ble_control_phase phase;
    bool stop;
    char host[1025], name[256], line[1536];
    unsigned port, width, height;
    bool middle_button_home;
    bool right_button_back;
    unsigned side_button_1_action;
    unsigned side_button_2_action;
    unsigned side_swipe_distance_percent;
    size_t used;
};
/* Commands are bounded UTF-8 lines: pause, stop, or
 * resume<TAB>host<TAB>port<TAB>screen<TAB>width<TAB>height<TAB>middle-action
 * [<TAB>right-action<TAB>side-1-action<TAB>side-2-action<TAB>distance-percent].
 * Legacy commands without the optional fields default to normal actions and
 * a 17 percent swipe distance.
 * Resume is accepted only after the release/drain acknowledgement. The peer
 * and HID descriptors are deliberately immutable for this provider lifetime. */
bool otg_ble_control_feed(struct otg_ble_control *c, const char *bytes, size_t size);
#endif
