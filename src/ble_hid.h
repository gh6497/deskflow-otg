/* Linux Bluetooth LE HID over GATT transport. SPDX-License-Identifier: Apache-2.0 */
#ifndef DESKFLOW_OTG_BLE_HID_H
#define DESKFLOW_OTG_BLE_HID_H

#include "bridge.h"

#ifdef __linux__
#include <stdbool.h>
#include <stdint.h>

struct otg_ble_hid;

struct otg_ble_hid *otg_ble_hid_create(const char *adapter);
struct otg_ble_hid *otg_ble_hid_create_resident(const char *adapter);
bool otg_ble_hid_set_session(struct otg_ble_hid *b, bool enabled);
void otg_ble_hid_set_interval(struct otg_ble_hid *b, uint64_t us);
bool otg_ble_hid_init(struct otg_ble_hid *b, const char *adapter);
void otg_ble_hid_destroy(struct otg_ble_hid *b);
const struct otg_hid_transport_ops *otg_ble_hid_ops(void);
const char *otg_ble_hid_adapter(const struct otg_ble_hid *b);

#endif
#endif
