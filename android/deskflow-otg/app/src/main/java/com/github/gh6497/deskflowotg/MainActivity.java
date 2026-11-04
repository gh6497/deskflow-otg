/*
 * deskflow-otg - Android companion application.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
package com.github.gh6497.deskflowotg;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.TextUtils;
import android.widget.TextView;
import android.widget.Switch;
import android.widget.Toast;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Setup and leased Deskflow connection-status screen.
 *
 * The notification-listener permission is required because a normal Android
 * application cannot call the shell-only media_session command. Once the
 * permission is enabled, Android binds DeskflowNotificationListenerService
 * automatically. The service handles USB removal and optional, user-selected
 * player resume on a fresh Deskflow connection event.
 */
public final class MainActivity extends Activity {
    private TextView status;
    private TextView connectionStatus;
    private TextView autoplayTarget;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refreshConnection = new Runnable() {
        @Override
        public void run() {
            String state = ConnectionStatusReceiver.current(MainActivity.this);
            int text = R.string.connection_unknown;
            if ("connecting".equals(state)) text = R.string.connection_connecting;
            else if ("connected".equals(state)) text = R.string.connection_connected;
            else if ("disconnecting".equals(state)) text = R.string.connection_disconnecting;
            else if ("disconnected".equals(state)) text = R.string.connection_disconnected;
            connectionStatus.setText(text);
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        status = findViewById(R.id.notification_status);
        connectionStatus = findViewById(R.id.connection_status);
        autoplayTarget = findViewById(R.id.autoplay_target);

        findViewById(R.id.notification_settings).setOnClickListener(view -> {
            startActivity(new Intent("android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"));
        });

        Switch autoplay = findViewById(R.id.autoplay_enabled);
        autoplay.setChecked(AutoplaySettings.enabled(this));
        autoplay.setOnCheckedChangeListener((button, checked) ->
                AutoplaySettings.prefs(this).edit().putBoolean("enabled", checked).apply());
        updateAutoplayTarget();
        findViewById(R.id.autoplay_select_player).setOnClickListener(view -> selectAutoplayPlayer());
        findViewById(R.id.open_transfers).setOnClickListener(view ->
                startActivity(new Intent(this, TransferActivity.class)));
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.removeCallbacks(refreshConnection);
        refreshConnection.run();
        if (status != null) {
            status.setText(notificationListenerEnabled()
                    ? R.string.status_access_enabled
                    : R.string.status_access_required);
        }
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refreshConnection);
        super.onPause();
    }

    private String playerLabel(String target) {
        try {
            PackageManager manager = getPackageManager();
            return manager.getApplicationLabel(manager.getApplicationInfo(target, 0))
                    + " (" + target + ")";
        } catch (PackageManager.NameNotFoundException exception) {
            return target;
        }
    }

    private void updateAutoplayTarget() {
        String target = AutoplaySettings.target(this);
        autoplayTarget.setText(target.isEmpty() ? getString(R.string.autoplay_no_target)
                : getString(R.string.autoplay_target, playerLabel(target)));
    }

    private void selectAutoplayPlayer() {
        if (!notificationListenerEnabled()) {
            Toast.makeText(this, R.string.status_access_required, Toast.LENGTH_LONG).show();
            return;
        }
        MediaSessionManager manager = (MediaSessionManager) getSystemService(Context.MEDIA_SESSION_SERVICE);
        if (manager == null) return;
        Set<String> packages = new LinkedHashSet<>();
        try {
            for (MediaController controller : manager.getActiveSessions(
                    new ComponentName(this, DeskflowNotificationListenerService.class))) {
                packages.add(controller.getPackageName());
            }
        } catch (RuntimeException exception) {
            Toast.makeText(this, R.string.autoplay_unavailable, Toast.LENGTH_LONG).show();
            return;
        }
        if (packages.isEmpty()) {
            Toast.makeText(this, R.string.autoplay_no_sessions, Toast.LENGTH_LONG).show();
            return;
        }
        String[] targets = packages.toArray(new String[0]);
        String[] labels = new String[targets.length];
        for (int i = 0; i < targets.length; ++i) labels[i] = playerLabel(targets[i]);
        new AlertDialog.Builder(this).setTitle(R.string.autoplay_select_player)
                .setItems(labels, (dialog, index) -> {
                    AutoplaySettings.prefs(this).edit().putString("target", targets[index]).apply();
                    updateAutoplayTarget();
                }).setNegativeButton(android.R.string.cancel, null).show();
    }

    private boolean notificationListenerEnabled() {
        String enabled = Settings.Secure.getString(
                getContentResolver(), "enabled_notification_listeners");
        if (TextUtils.isEmpty(enabled)) return false;
        ComponentName component = new ComponentName(this,
                DeskflowNotificationListenerService.class);
        for (String value : enabled.split(":")) {
            if (component.equals(ComponentName.unflattenFromString(value))) return true;
        }
        return false;
    }
}
