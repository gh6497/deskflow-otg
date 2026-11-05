/* Android wm/InputReader display discovery; no video or extra scrcpy socket.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_ADB_DISPLAY_H
#define OTG_ADB_DISPLAY_H
#include <stdbool.h>

struct otg_display_sample { int width, height, rotation; };
struct otg_display_size { int width, height, virtual_width, virtual_height; };
bool otg_display_parse(const char *text, struct otg_display_sample *sample);

struct otg_display_monitor;
/* No ADB command is run until poll. All queries are asynchronous, bounded and
 * directed at this exact transport. The initial virtual axes retain scaling. */
struct otg_display_monitor *otg_display_monitor_create(const char *adb, const char *serial,
                                                      int width, int height);
bool otg_display_monitor_poll(struct otg_display_monitor *monitor, struct otg_display_size *size);
void otg_display_monitor_request_lock(struct otg_display_monitor *monitor);
bool otg_display_monitor_take_lock_state(struct otg_display_monitor *monitor,
                                         bool *valid, bool *locked);
void otg_display_monitor_prefer_companion(struct otg_display_monitor *monitor);
void otg_display_monitor_destroy(struct otg_display_monitor *monitor);
#endif
