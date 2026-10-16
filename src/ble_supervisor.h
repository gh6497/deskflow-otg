/* Linux native BLE isolation supervisor. SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_SUPERVISOR_H
#define OTG_BLE_SUPERVISOR_H
#include <signal.h>
int otg_ble_supervise(int argc, char **argv, const char *adapter,
                      int restore_only, volatile sig_atomic_t *stop);
#endif
