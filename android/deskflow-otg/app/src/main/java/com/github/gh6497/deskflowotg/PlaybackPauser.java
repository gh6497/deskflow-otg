/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

/** Iterate a session snapshot, isolating stale or failing players from others. */
final class PlaybackPauser {
    interface Actions<T> {
        boolean isPlaying(T session);
        void pause(T session);
        void failed(T session, RuntimeException exception);
    }

    private PlaybackPauser() {}

    static <T> int pauseAll(Iterable<T> sessions, Actions<T> actions) {
        int requested = 0;
        for (T session : sessions) {
            try {
                if (!actions.isPlaying(session)) continue;
                actions.pause(session);
                requested++;
            } catch (RuntimeException exception) {
                actions.failed(session, exception);
            }
        }
        return requested;
    }
}
