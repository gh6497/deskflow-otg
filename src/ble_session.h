/* Native resident GATT service and local bridge sessions.
 * BlueZ owns CCCs; keeping its application registered preserves their lifetime.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_SESSION_H
#define OTG_BLE_SESSION_H
#include "bridge.h"
#include <signal.h>
#include <sys/types.h>
struct otg_ble_session;
int otg_ble_service_run(const char *adapter, volatile sig_atomic_t *stop);
struct otg_ble_session *otg_ble_session_open(const char *adapter);
const struct otg_hid_transport_ops *otg_ble_session_ops(void);
void otg_ble_session_destroy(struct otg_ble_session *session);
/* Supervisor's read-only barrier before restoring modes after a child exits. */
bool otg_ble_service_wait_idle(uid_t uid, const char *adapter);
#endif
