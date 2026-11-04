/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.SystemClock;
import android.provider.Settings;

/** Explicit ADB shell reports; the manifest requires the sender's DUMP permission. */
public final class ConnectionStatusReceiver extends BroadcastReceiver {
    private static final String ACTION = "com.github.gh6497.deskflowotg.CONNECTION_STATUS";
    private static final String PREFS = "connection_status";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())
                || intent.getIntExtra("version", 0) != 1) return;
        String state = intent.getStringExtra("state");
        if (!ConnectionStatus.valid(state)) return;
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString("state", state)
                .putLong("received", SystemClock.elapsedRealtime())
                .putInt("boot", bootCount(context)).apply();
    }

    static String current(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        return ConnectionStatus.current(prefs.getString("state", "unknown"),
                prefs.getLong("received", -1), prefs.getInt("boot", -1),
                SystemClock.elapsedRealtime(), bootCount(context));
    }

    private static int bootCount(Context context) {
        return Settings.Global.getInt(context.getContentResolver(), Settings.Global.BOOT_COUNT, -1);
    }
}
