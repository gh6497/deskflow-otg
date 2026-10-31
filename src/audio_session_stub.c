/* Stub for platforms without a native audio backend.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_session.h"

#include <stdio.h>
#include <string.h>

int audio_session_start(struct audio_session *session, const char *adb_path,
                        const char *serial, const char *server_path,
                        const char *server_version)
{
    (void)session;
    (void)adb_path;
    (void)serial;
    (void)server_path;
    (void)server_version;
    fprintf(stderr, "ERROR: audio forwarding is not enabled in this build\n");
    return -1;
}

void audio_session_stop(struct audio_session *session)
{
    if (session) memset(session, 0, sizeof(*session));
}

bool audio_session_started(const struct audio_session *session)
{
    return session && session->started;
}
