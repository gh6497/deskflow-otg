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
import android.provider.Settings;
import android.text.TextUtils;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Small setup screen for the first release.
 *
 * The notification-listener permission is required because a normal Android
 * application cannot call the shell-only media_session command. Once the
 * permission is enabled, Android binds DeskflowNotificationListenerService
 * automatically and the service listens for USB accessory removal.
 */
public final class MainActivity extends Activity {
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(createContentView());
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (status != null) {
            status.setText(notificationListenerEnabled()
                    ? R.string.status_access_enabled
                    : R.string.status_access_required);
        }
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
