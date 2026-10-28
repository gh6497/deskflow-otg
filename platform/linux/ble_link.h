/* Session-only Linux HCI link timing monitor. SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_LINK_H
#define OTG_BLE_LINK_H
#include <stdint.h>
struct otg_ble_link;
struct otg_ble_link *otg_ble_link_open(uint16_t index,int channel);
void otg_ble_link_poll(struct otg_ble_link *link);
void otg_ble_link_close(struct otg_ble_link *link);
#endif
