/* Exercise the real ADB/server lifetime against fake_scrcpy_adb.py, no devices.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "audio_session.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int copied;
static void on_text(void *ud, const char *text, size_t size)
{
    (void)ud;
    if (size == 7 && !memcmp(text, "android", 7)) ++copied;
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    struct audio_session s = {0};
    /* Stopping an unused or failed session must not close stdin/socket 0. */
    audio_session_stop(&s);
    bool audio = !strcmp(argv[2], "audio") || !strcmp(argv[2], "both");
    bool clipboard = strcmp(argv[2], "audio") != 0;
    int result = audio_session_start_features(&s, argv[1], "FAKE_PHONE", "fake-server", "5.0.1", audio, clipboard);
    if (!strcmp(argv[2], "fail")) {
        audio_session_stop(&s);
        return result == -1 ? 0 : 1;
    }
    if (result || !audio_session_started(&s)) return 1;
    if (clipboard) {
        if (otg_clipboard_channel_set(&s.clipboard, "computer", 8)) return 1;
        for (unsigned i = 0; i < 500; ++i) {
            if (otg_clipboard_channel_poll(&s.clipboard, on_text, NULL)) return 1;
            if (copied && !otg_clipboard_channel_busy(&s.clipboard)) break;
            const struct timespec delay = {0, 5000000}; nanosleep(&delay, NULL);
        }
        if (copied != 1 || otg_clipboard_channel_busy(&s.clipboard)) return 1;
    }
    audio_session_stop(&s);
    audio_session_stop(&s);
    if (audio_session_started(&s)) return 1;
    puts("PASS: one server/tunnel, independent streams, clipboard ACK and cleanup");
    return 0;
}
