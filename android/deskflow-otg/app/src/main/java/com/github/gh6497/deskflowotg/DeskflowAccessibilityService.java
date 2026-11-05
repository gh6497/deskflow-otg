/*
 * deskflow-otg - Android companion application.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
package com.github.gh6497.deskflowotg;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.view.accessibility.AccessibilityEvent;
import android.util.Log;
import android.view.KeyEvent;

/**
 * Converts reserved F13/F14 HID shortcuts into Android's global Home/Back actions.
 * The desktop bridge emits these shortcuts for the configured middle/right
 * button mappings; ordinary mouse buttons are untouched.
 */
public final class DeskflowAccessibilityService extends AccessibilityService {
    private static final String TAG = "deskflow-otg";
    // Android <= 15 leaves F13/F14 unmapped in Generic.kl. The Linux HID
    // keyboard driver still emits these evdev scan codes for both USB and BLE.
    private static final int LINUX_KEY_F13 = 183;
    private static final int LINUX_KEY_F14 = 184;

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        AccessibilityServiceInfo info = getServiceInfo();
        if (info != null) {
            info.flags |= AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS;
            setServiceInfo(info);
        }
        Log.i(TAG, "Home/Back shortcut accessibility service connected");
    }

    @Override
    public boolean onKeyEvent(KeyEvent event) {
        final int keyCode = event.getKeyCode();
        final int scanCode = event.getScanCode();
        final int action;
        if (keyCode == KeyEvent.KEYCODE_F13 ||
                (keyCode == KeyEvent.KEYCODE_UNKNOWN && scanCode == LINUX_KEY_F13)) {
            action = GLOBAL_ACTION_HOME;
        } else if (keyCode == KeyEvent.KEYCODE_F14 ||
                (keyCode == KeyEvent.KEYCODE_UNKNOWN && scanCode == LINUX_KEY_F14)) {
            action = GLOBAL_ACTION_BACK;
        } else {
            return super.onKeyEvent(event);
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            final boolean performed = performGlobalAction(action);
            final String name = action == GLOBAL_ACTION_HOME ? "Home" : "Back";
            // Only log reserved shortcuts, never ordinary keyboard input.
            Log.i(TAG, "HID shortcut " + name + " keyCode=" + keyCode +
                    " scanCode=" + scanCode + " requested=" + performed);
            if (!performed) {
                Log.w(TAG, "could not perform Android " + name + " action");
            }
        }
        // Consume down, repeats and up so the shortcut never reaches the foreground app.
        return true;
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        // The service only needs the key-event filter; window events are ignored.
    }

    @Override
    public void onInterrupt() {
        Log.i(TAG, "Home/Back shortcut accessibility service interrupted");
    }
}
