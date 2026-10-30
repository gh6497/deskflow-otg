/*
 * audio_session.h - scrcpy-compatible Android audio forwarding.
 *
 * The Android side is the official scrcpy server.  This module only owns the
 * ADB tunnel, parses scrcpy's raw audio stream and sends it to the host audio
 * device.  Keeping this boundary compatible with scrcpy lets us replace RAW
 * with OPUS later without changing session lifetime management.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_AUDIO_SESSION_H
#define DESKFLOW_OTG_AUDIO_SESSION_H

#include <stdbool.h>

struct audio_session {
    bool started;
    bool stop_requested;
    int socket_fd;
    int server_pid;
    int forward_port;
    char *adb_path;
    char *serial;
    char *server_path;
    void *thread;
    void *pcm;
};

/*
 * Start a scrcpy server in audio-only/raw mode and play its stream locally.
 * The server_path must point to a scrcpy-server jar/apk matching the installed
 * scrcpy server implementation.  Audio is currently supported on Linux only,
 * where the output uses ALSA; other platforms return an explanatory error.
 */
int audio_session_start(struct audio_session *session,
                        const char *adb_path,
                        const char *serial,
                        const char *server_path,
                        const char *server_version);

void audio_session_stop(struct audio_session *session);

bool audio_session_started(const struct audio_session *session);

#endif /* DESKFLOW_OTG_AUDIO_SESSION_H */
