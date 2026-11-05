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
 * Converts the reserved F13 HID shortcut into Android's global Home action.
 * The desktop bridge emits F13 only when the user selects this middle-button
 * mapping, so ordinary keyboards and ordinary mouse buttons are untouched.
 */
public final class DeskflowAccessibilityService extends AccessibilityService {
    private static final String TAG = "deskflow-otg";

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        AccessibilityServiceInfo info = getServiceInfo();
        if (info != null) {
            info.flags |= AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS;
            setServiceInfo(info);
        }
        Log.i(TAG, "Home shortcut accessibility service connected");
    }

    @Override
    public boolean onKeyEvent(KeyEvent event) {
        if (event.getKeyCode() != KeyEvent.KEYCODE_F13) {
            return super.onKeyEvent(event);
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (!performGlobalAction(GLOBAL_ACTION_HOME)) {
                Log.w(TAG, "could not perform Android Home action");
            }
        }
        // Consume both down and up so F13 never reaches the foreground app.
        return true;
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        // The service only needs the key-event filter; window events are ignored.
    }

    @Override
    public void onInterrupt() {
        Log.i(TAG, "Home shortcut accessibility service interrupted");
    }
}
