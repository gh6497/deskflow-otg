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
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.ScrollView;
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
        ScrollView scroll = new ScrollView(this);
        scroll.addView(createContentView());
        setContentView(scroll);
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

    private LinearLayout createContentView() {
        int padding = (int) (getResources().getDisplayMetrics().density * 24.0f);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(padding, padding, padding, padding);

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(28.0f);
        title.setTextColor(0xff1565c0);
        root.addView(title, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        connectionStatus = new TextView(this);
        connectionStatus.setText(R.string.connection_unknown);
        connectionStatus.setTextSize(22.0f);
        LinearLayout.LayoutParams connectionParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        connectionParams.topMargin = padding;
        root.addView(connectionStatus, connectionParams);

        TextView connectionHelp = new TextView(this);
        connectionHelp.setText(R.string.connection_help);
        root.addView(connectionHelp, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView description = new TextView(this);
        description.setText(R.string.setup_description);
        description.setTextSize(16.0f);
        LinearLayout.LayoutParams descriptionParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        descriptionParams.topMargin = padding / 2;
        root.addView(description, descriptionParams);

        status = new TextView(this);
        status.setTextSize(15.0f);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        statusParams.topMargin = padding;
        root.addView(status, statusParams);

        Button settingsButton = new Button(this);
        settingsButton.setText(R.string.open_notification_access_settings);
        settingsButton.setOnClickListener(view -> {
            startActivity(new Intent("android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"));
        });
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        buttonParams.topMargin = padding;
        root.addView(settingsButton, buttonParams);

        Switch autoplay = new Switch(this);
        autoplay.setText(R.string.autoplay_enabled);
        autoplay.setChecked(AutoplaySettings.enabled(this));
        autoplay.setOnCheckedChangeListener((button, checked) ->
                AutoplaySettings.prefs(this).edit().putBoolean("enabled", checked).apply());
        root.addView(autoplay, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView autoplayHelp = new TextView(this);
        autoplayHelp.setText(R.string.autoplay_help);
        root.addView(autoplayHelp);
        autoplayTarget = new TextView(this);
        updateAutoplayTarget();
        root.addView(autoplayTarget);
        Button selectPlayer = new Button(this);
        selectPlayer.setText(R.string.autoplay_select_player);
        selectPlayer.setOnClickListener(view -> selectAutoplayPlayer());
        root.addView(selectPlayer);

        Button transfers = new Button(this);
        transfers.setText(R.string.transfer_title);
        transfers.setOnClickListener(view -> startActivity(new Intent(this, TransferActivity.class)));
        root.addView(transfers);

        return root;
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
