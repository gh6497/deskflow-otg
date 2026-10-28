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
    size_t used;
};
/* Commands are bounded UTF-8 lines: pause, stop, or
 * resume<TAB>host<TAB>port<TAB>screen<TAB>width<TAB>height.
 * Resume is accepted only after the release/drain acknowledgement. The peer
 * and HID descriptors are deliberately immutable for this provider lifetime. */
bool otg_ble_control_feed(struct otg_ble_control *c, const char *bytes, size_t size);
#endif
