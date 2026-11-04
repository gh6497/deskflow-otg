/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

/** Only a new, explicit connection edge can request play; never a lease renewal. */
final class AutoplayPolicy {
    static boolean shouldConsume(String id, String state, boolean ready,
                                 boolean event, boolean consumed) {
        return event && ready && "connected".equals(state) && !consumed
                && id != null && id.matches("[0-9a-f]{32}");
    }

    interface Actions<T> {
        String packageName(T session);
        boolean canResume(T session);
        void play(T session);
        void failed(T session, RuntimeException exception);
    }

    static <T> boolean playOne(Iterable<T> sessions, String target, Actions<T> actions) {
        if (target == null || target.isEmpty()) return false;
        for (T session : sessions) {
            try {
                if (!target.equals(actions.packageName(session)) || !actions.canResume(session)) continue;
            } catch (RuntimeException exception) {
                actions.failed(session, exception);
                continue;
            }
            // At most one play attempt, including a throwing/stale controller.
            try {
                actions.play(session);
                return true;
            } catch (RuntimeException exception) {
                actions.failed(session, exception);
                return false;
            }
        }
        return false;
    }

    private AutoplayPolicy() {}
}
