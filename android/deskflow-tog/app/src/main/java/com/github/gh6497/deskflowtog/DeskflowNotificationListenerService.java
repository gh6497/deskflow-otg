/*
 * deskflow-tog - Android companion application.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
package com.github.gh6497.deskflowtog;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbManager;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.IBinder;
import android.service.notification.NotificationListenerService;
import android.util.Log;

import java.util.List;

/**
 * Receives the system USB accessory removal broadcast and pauses the highest
 * priority currently playing media session.
 *
 * A notification listener is used instead of shell commands or injected media
 * key events. This keeps the operation inside Android's supported permission
 * model and works after the host-side USB connection has disappeared.
 */
public final class DeskflowNotificationListenerService extends NotificationListenerService {
    private static final String TAG = "deskflow-tog";

    private boolean detachReceiverRegistered;
    private final BroadcastReceiver detachReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(intent.getAction())) {
                pauseCurrentPlayback();
            }
        }
    };

    @Override
    public void onListenerConnected() {
        super.onListenerConnected();
        registerDetachReceiver();
        Log.i(TAG, "media pause listener connected");
    }

    @Override
    public void onListenerDisconnected() {
        unregisterDetachReceiver();
        Log.i(TAG, "media pause listener disconnected");
        super.onListenerDisconnected();
    }

    @Override
    public void onDestroy() {
        unregisterDetachReceiver();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return super.onBind(intent);
    }

    private void registerDetachReceiver() {
        if (detachReceiverRegistered) return;
        IntentFilter filter = new IntentFilter(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            registerReceiver(detachReceiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(detachReceiver, filter);
        }
        detachReceiverRegistered = true;
    }

    private void unregisterDetachReceiver() {
        if (!detachReceiverRegistered) return;
        unregisterReceiver(detachReceiver);
        detachReceiverRegistered = false;
    }

    private void pauseCurrentPlayback() {
        MediaSessionManager manager = (MediaSessionManager) getSystemService(
                Context.MEDIA_SESSION_SERVICE);
        if (manager == null) {
            Log.w(TAG, "media session service is unavailable");
            return;
        }

        ComponentName listener = new ComponentName(this, getClass());
        List<MediaController> sessions;
        try {
            sessions = manager.getActiveSessions(listener);
        } catch (SecurityException exception) {
            Log.w(TAG, "notification listener permission is no longer available", exception);
            return;
        }

        for (MediaController controller : sessions) {
            PlaybackState state = controller.getPlaybackState();
            if (state == null || !isPlaying(state.getState())) continue;
            try {
                controller.getTransportControls().pause();
                Log.i(TAG, "pause requested for " + controller.getPackageName());
            } catch (RuntimeException exception) {
                Log.w(TAG, "media session rejected pause request", exception);
            }
            return;
        }
        Log.i(TAG, "USB accessory detached; no active playback session");
    }

    private static boolean isPlaying(int state) {
        return state == PlaybackState.STATE_PLAYING
                || state == PlaybackState.STATE_BUFFERING;
    }

}
