/*
 * audio_session.h - shared scrcpy ADB session (audio and optional clipboard).
 *
 * The Android side is the official scrcpy server.  This module only owns the
 * ADB tunnel and independent audio/control sockets. Audio is played locally;
 * clipboard messages are pumped by the Deskflow thread, without OS clipboard
 * access. Keeping this boundary compatible with scrcpy lets us replace RAW
 * with OPUS later without changing session lifetime management.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_AUDIO_SESSION_H
#define DESKFLOW_OTG_AUDIO_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include "clipboard.h"

struct audio_session {
    bool started;
    bool stop_requested;
    intptr_t socket_fd;
    intptr_t server_pid;
    int forward_port;
    char *adb_path;
    char *adb_serial;
    char *server_path;
    void *thread;
    void *pcm;
    bool initialized;
    bool audio_enabled;
    bool winsock_ready;
    intptr_t control_fd;
    struct otg_clipboard_channel clipboard;
};

/*
 * Start a scrcpy server in audio-only/raw mode and play its stream locally.
 * The server_path must point to a scrcpy-server jar/apk matching the installed
 * scrcpy server implementation. Linux uses ALSA and Windows uses WASAPI.
 */
int audio_session_start(struct audio_session *session,
                        const char *adb_path,
                        const char *adb_serial,
                        const char *server_path,
                        const char *server_version);

/* One ADB/server lifetime, with independent audio and clipboard sockets. */
int audio_session_start_features(struct audio_session *session,
                        const char *adb_path, const char *adb_serial,
                        const char *server_path, const char *server_version,
                        bool audio, bool clipboard);

void audio_session_stop(struct audio_session *session);

bool audio_session_started(const struct audio_session *session);

#endif /* DESKFLOW_OTG_AUDIO_SESSION_H */
