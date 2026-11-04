/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

/** Pure policy: only a recent host report in the same boot is authoritative. */
final class ConnectionStatus {
    static final long LEASE_MS = 15000;

    static boolean valid(String state) {
        return "connecting".equals(state) || "connected".equals(state)
                || "disconnecting".equals(state) || "disconnected".equals(state);
    }

    static String current(String state, long received, int boot, long now, int currentBoot) {
        if (!valid(state) || boot < 0 || boot != currentBoot || received < 0
                || now < received || now - received >= LEASE_MS) return "unknown";
        return state;
    }

    private ConnectionStatus() {}
}
