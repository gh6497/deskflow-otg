/*
 * scrcpy_session.h - shared scrcpy ADB session (audio and optional clipboard).
 *
 * The Android side is the official scrcpy server.  This module only owns the
 * ADB tunnel and independent audio/control sockets. Audio playback is delegated
 * to the platform audio_player module;
 * clipboard messages are pumped by the Deskflow thread, without OS clipboard
 * access. Keeping this boundary compatible with scrcpy lets us replace RAW
 * with OPUS later without changing session lifetime management.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_SCRCPY_SESSION_H
#define DESKFLOW_OTG_SCRCPY_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include "clipboard.h"

struct audio_player;

/* Owns the server, tunnel and sockets. The optional player borrows audio_fd;
 * clipboard protocol state is driven separately by the Deskflow thread. */
struct scrcpy_session {
    bool started;
    intptr_t audio_fd;
    intptr_t server_pid;
    int forward_port;
    char *adb_path;
    char *adb_serial;
    char *server_path;
    struct audio_player *player;
    bool initialized;
    bool winsock_ready;
    intptr_t control_fd;
    struct otg_clipboard_channel clipboard;
};

/*
 * Start one scrcpy server/tunnel with independent audio and control sockets.
 * At least one feature must be requested. Stop before restarting a session.
 * The server_path must point to a scrcpy-server jar/apk matching the installed
 * scrcpy server implementation.
 */
int scrcpy_session_start(struct scrcpy_session *session,
                        const char *adb_path, const char *adb_serial,
                        const char *server_path, const char *server_version,
                        bool audio, bool clipboard);

void scrcpy_session_stop(struct scrcpy_session *session);

bool scrcpy_session_started(const struct scrcpy_session *session);

#endif /* DESKFLOW_OTG_SCRCPY_SESSION_H */
