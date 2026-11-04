/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

/** USB disconnect policy, independent of Android so event sequences are testable. */
final class UsbDisconnectMonitor {
    private boolean initialized;
    private boolean connected;
    private boolean handled;

    /** Returns true exactly when this event should immediately pause playback. */
    boolean usbState(boolean nowConnected) {
        if (nowConnected) {
            initialized = connected = true;
            handled = false;
            return false;
        }
        if (!initialized) {
            initialized = true;
            // A sticky disconnected state at service startup is not an unplug.
            return false;
        }
        if (!connected) return false;
        connected = false;
        return handleDisconnect();
    }

    boolean accessoryDetached() {
        // Retain the original accessory-mode path on devices without USB_STATE.
        // Without a connection baseline, separate detach events cannot be
        // distinguished from duplicates; handle each rather than miss an unplug.
        return !initialized || handleDisconnect();
    }

    private boolean handleDisconnect() {
        if (handled) return false;
        handled = true;
        return true;
    }

    void reset() {
        initialized = connected = handled = false;
    }
}
