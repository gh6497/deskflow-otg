/* Windows BLE HID transport (C ABI over C++/WinRT). SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_WIN_H
#define OTG_BLE_WIN_H
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
struct otg_ble_win;
struct otg_hid_transport_ops;

/* peer is the paired phone address reported by Windows, AA:BB:CC:DD:EE:FF. */
struct otg_ble_win *otg_ble_win_create(const char *peer);
const struct otg_hid_transport_ops *otg_ble_win_ops(void);
void otg_ble_win_destroy(struct otg_ble_win *ble);
/* Discard unsent actions and queue releases AFTER any in-flight report.
 * The main thread must stop Deskflow input before calling this. GATT and CCCs
 * remain registered. Idle means drained or link lost, not guaranteed delivery. */
void otg_ble_win_release_inputs(struct otg_ble_win *ble);
bool otg_ble_win_input_idle(struct otg_ble_win *ble);

#ifdef __cplusplus
}
#endif
#endif
