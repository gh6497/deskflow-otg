/*
 * deskflow-otg - Android companion application.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
package com.github.gh6497.deskflowotg;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
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

/**
 * Setup and leased Deskflow connection-status screen.
 *
 * The notification-listener permission is required because a normal Android
 * application cannot call the shell-only media_session command. Once the
 * permission is enabled, Android binds DeskflowNotificationListenerService
 * automatically and the service listens for USB accessory removal.
 */
public final class MainActivity extends Activity {
    private TextView status;
    private TextView connectionStatus;
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

        return root;
    }

    private boolean notificationListenerEnabled() {
        String enabled = Settings.Secure.getString(
                getContentResolver(), "enabled_notification_listeners");
        if (TextUtils.isEmpty(enabled)) return false;
        ComponentName component = new ComponentName(this,
                DeskflowNotificationListenerService.class);
        for (String value : enabled.split(":")) {
            if (component.flattenToString().equals(value)) return true;
        }
        return false;
    }
}
