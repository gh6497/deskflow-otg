/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbManager;
import android.util.Log;

/** Receives USB removal even when the notification listener is not currently bound. */
public final class UsbDisconnectReceiver extends BroadcastReceiver {
    private static final String TAG = "deskflow-otg";
    private static final String ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE";
    private static final String EXTRA_CONNECTED = "connected";
    private static final String PREFS = "usb_disconnect";
    private static final String INITIALIZED = "initialized";
    private static final String CONNECTED = "connected";
    private static final String HANDLED = "handled";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null) return;

        final boolean shouldPause;
        final String action = intent.getAction();
        if (ACTION_USB_STATE.equals(action) && intent.hasExtra(EXTRA_CONNECTED)) {
            shouldPause = updateUsbState(context, intent.getBooleanExtra(EXTRA_CONNECTED, false));
            Log.i(TAG, "USB state connected=" + intent.getBooleanExtra(EXTRA_CONNECTED, false));
        } else if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(action)) {
            shouldPause = updateAccessoryDetached(context);
            Log.i(TAG, "USB accessory detached");
        } else {
            return;
        }

        if (shouldPause) {
            Log.i(TAG, "USB disconnected; pausing playback immediately");
            DeskflowNotificationListenerService.pauseAllPlayback(context);
        }
    }

    private static boolean updateUsbState(Context context, boolean nowConnected) {
        final android.content.SharedPreferences preferences =
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        final UsbDisconnectMonitor monitor = restore(preferences);
        final boolean shouldPause = monitor.usbState(nowConnected);
        save(preferences, monitor);
        return shouldPause;
    }

    private static boolean updateAccessoryDetached(Context context) {
        final android.content.SharedPreferences preferences =
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        final UsbDisconnectMonitor monitor = restore(preferences);
        final boolean shouldPause = monitor.accessoryDetached();
        save(preferences, monitor);
        return shouldPause;
    }

    private static UsbDisconnectMonitor restore(android.content.SharedPreferences preferences) {
        final UsbDisconnectMonitor monitor = new UsbDisconnectMonitor();
        monitor.restore(preferences.getBoolean(INITIALIZED, false),
                preferences.getBoolean(CONNECTED, false),
                preferences.getBoolean(HANDLED, false));
        return monitor;
    }

    private static void save(android.content.SharedPreferences preferences,
                             UsbDisconnectMonitor monitor) {
        preferences.edit()
                .putBoolean(INITIALIZED, monitor.isInitialized())
                .putBoolean(CONNECTED, monitor.isConnected())
                .putBoolean(HANDLED, monitor.isHandled())
                .apply();
    }
}
