/* ADB target preparation; reference: Android adb connect/get-state commands.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_ADB_TARGET_H
#define OTG_ADB_TARGET_H

#include <stdbool.h>
#include <stddef.h>

bool otg_adb_target_is_tcp(const char *serial);
bool otg_adb_state_is_device(const char *output, size_t size);

/* connect=false: run -s target get-state, return 0 only for state "device".
 * connect=true: run connect target, return 0 only on process success.
 * Implementations must bound child lifetime and preserve the configured target. */
typedef int (*otg_adb_target_command)(void *ud, const char *target, bool connect);

/* Every target is checked before server push. Explicit host:port targets are
 * connected once if needed, then checked again. USB/mDNS targets must already
 * be online. Never pairs, disconnects, switches transport, or restarts the
 * global ADB server. */
int otg_adb_target_prepare(const char *serial, otg_adb_target_command command, void *ud);

#endif
