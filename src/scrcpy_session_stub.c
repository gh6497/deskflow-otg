/* Stub for builds without scrcpy ADB features.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scrcpy_session.h"

#include <stdio.h>
#include <string.h>

int scrcpy_session_start(struct scrcpy_session *session, const char *adb_path,
                          const char *adb_serial, const char *server_path,
                          const char *server_version, bool audio, bool clipboard)
{
    (void)session;
    (void)adb_path;
    (void)adb_serial;
    (void)server_path;
    (void)server_version;
    (void)audio;
    (void)clipboard;
    fprintf(stderr, "ERROR: scrcpy ADB features are not enabled in this build\n");
    return -1;
}

void scrcpy_session_stop(struct scrcpy_session *session)
{
    if (session) memset(session, 0, sizeof(*session));
}

bool scrcpy_session_started(const struct scrcpy_session *session)
{
    return session && session->started;
}
