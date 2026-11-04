/*
 * deskflow-otg - Android companion application.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
package com.github.gh6497.deskflowotg;

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
 * Receives USB link/accessory removal broadcasts and requests pause for every
 * currently playing or buffering media session.
 * Also resumes one explicitly selected paused session on an opt-in connection
 * edge, but never starts itself or replays an event after listener rebinding.
 *
 * A notification listener is used instead of shell commands or injected media
 * key events. This keeps the operation inside Android's supported permission
 * model and works after the host-side USB connection has disappeared.
 */
public final class DeskflowNotificationListenerService extends NotificationListenerService {
    private static final String TAG = "deskflow-otg";
    // Receiver and service lifecycle callbacks run on the application main thread.
    // Never start/bind a service or replay a persisted request to obtain this.
    private static DeskflowNotificationListenerService connectedListener;
    // Android broadcasts this protected sticky action for USB gadget links,
    // including AOA HID-only. Its constant is not part of the public SDK.
    private static final String ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE";
    private static final String EXTRA_CONNECTED = "connected";

    private final UsbDisconnectMonitor usbMonitor = new UsbDisconnectMonitor();
    private boolean detachReceiverRegistered;
    private final BroadcastReceiver detachReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!detachReceiverRegistered) return;
            final boolean shouldPause;
            if (ACTION_USB_STATE.equals(intent.getAction()) && intent.hasExtra(EXTRA_CONNECTED)) {
                final boolean connected = intent.getBooleanExtra(EXTRA_CONNECTED, false);
                shouldPause = usbMonitor.usbState(connected);
                Log.i(TAG, "USB state connected=" + connected
                        + " initial=" + isInitialStickyBroadcast());
            } else if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(intent.getAction())) {
                shouldPause = usbMonitor.accessoryDetached();
                Log.i(TAG, "USB accessory detached");
            } else {
                return;
            }
            if (shouldPause) {
                Log.i(TAG, "USB disconnected; pausing playback immediately");
                pauseAllPlayback();
            }
        }
    };

    @Override
    public void onListenerConnected() {
        super.onListenerConnected();
        connectedListener = this;
        registerDetachReceiver();
        Log.i(TAG, "media pause listener connected");
    }

    @Override
    public void onListenerDisconnected() {
        if (connectedListener == this) connectedListener = null;
        unregisterDetachReceiver();
        Log.i(TAG, "media pause listener disconnected");
        super.onListenerDisconnected();
    }

    @Override
    public void onDestroy() {
        if (connectedListener == this) connectedListener = null;
        unregisterDetachReceiver();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return super.onBind(intent);
    }

    private void registerDetachReceiver() {
        if (detachReceiverRegistered) return;
        usbMonitor.reset();
        IntentFilter filter = new IntentFilter(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        filter.addAction(ACTION_USB_STATE);
        final Intent initial;
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            initial = registerReceiver(detachReceiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            initial = registerReceiver(detachReceiver, filter);
        }
        detachReceiverRegistered = true;
        // Initialize before queued receiver callbacks; never pause on startup.
        if (initial != null && ACTION_USB_STATE.equals(initial.getAction())
                && initial.hasExtra(EXTRA_CONNECTED)) {
            usbMonitor.usbState(initial.getBooleanExtra(EXTRA_CONNECTED, false));
        }
    }

    private void unregisterDetachReceiver() {
        usbMonitor.reset();
        if (!detachReceiverRegistered) return;
        unregisterReceiver(detachReceiver);
        detachReceiverRegistered = false;
    }

    private void pauseAllPlayback() {
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

        final int requested = PlaybackPauser.pauseAll(sessions, new PlaybackPauser.Actions<MediaController>() {
            @Override
            public boolean isPlaying(MediaController controller) {
                final PlaybackState state = controller.getPlaybackState();
                return state != null && (state.getState() == PlaybackState.STATE_PLAYING
                        || state.getState() == PlaybackState.STATE_BUFFERING);
            }

            @Override
            public void pause(MediaController controller) {
                controller.getTransportControls().pause();
                Log.i(TAG, "pause requested for " + controller.getPackageName());
            }

            @Override
            public void failed(MediaController controller, RuntimeException exception) {
                Log.w(TAG, "could not inspect or pause media session for "
                        + controller.getPackageName(), exception);
            }
        });
        // A request is not proof that every player has actually paused.
        Log.i(TAG, "USB disconnected; pause requests sent=" + requested);
    }

    static void playOnConnection(Context context) {
        if (!AutoplaySettings.enabled(context)) return;
        DeskflowNotificationListenerService listener = connectedListener;
        if (listener == null) {
            Log.i(TAG, "auto-play skipped: notification listener is not connected");
            return;
        }
        String target = AutoplaySettings.target(context);
        if (target.isEmpty()) return;
        MediaSessionManager manager = (MediaSessionManager) listener.getSystemService(
                Context.MEDIA_SESSION_SERVICE);
        if (manager == null) return;
        try {
            List<MediaController> sessions = manager.getActiveSessions(
                    new ComponentName(listener, DeskflowNotificationListenerService.class));
            boolean requested = AutoplayPolicy.playOne(sessions, target,
                    new AutoplayPolicy.Actions<MediaController>() {
                @Override
                public String packageName(MediaController controller) {
                    return controller.getPackageName();
                }

                @Override
                public boolean canResume(MediaController controller) {
                    PlaybackState state = controller.getPlaybackState();
                    return state != null && state.getState() == PlaybackState.STATE_PAUSED
                            && (state.getActions() & PlaybackState.ACTION_PLAY) != 0;
                }

                @Override
                public void play(MediaController controller) {
                    controller.getTransportControls().play();
                    Log.i(TAG, "connection auto-play requested for " + target);
                }

                @Override
                public void failed(MediaController controller, RuntimeException exception) {
                    Log.w(TAG, "could not inspect or resume selected player", exception);
                }
            });
            if (!requested) Log.i(TAG, "auto-play skipped: no resumable selected session");
        } catch (RuntimeException exception) {
            Log.w(TAG, "auto-play unavailable; no retry for this connection", exception);
        }
    }

}
