/* Windows BLE HID transport (C ABI over C++/WinRT). SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_WIN_H
#define OTG_BLE_WIN_H

#ifdef __cplusplus
extern "C" {
#endif
struct otg_ble_win;
struct otg_hid_transport_ops;

/* peer is the paired phone address reported by Windows, AA:BB:CC:DD:EE:FF. */
struct otg_ble_win *otg_ble_win_create(const char *peer);
const struct otg_hid_transport_ops *otg_ble_win_ops(void);
void otg_ble_win_destroy(struct otg_ble_win *ble);

#ifdef __cplusplus
}
#endif
#endif
