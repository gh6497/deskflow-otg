/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.SystemClock;
import android.provider.Settings;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;

/** Explicit ADB shell reports; the manifest requires the sender's DUMP permission. */
public final class ConnectionStatusReceiver extends BroadcastReceiver {
    private static final String ACTION = "com.github.gh6497.deskflowotg.CONNECTION_STATUS";
    private static final String PREFS = "connection_status";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())) return;
        int version = intent.getIntExtra("version", 0);
        if (version != 1 && version != 2) return;
        String state = intent.getStringExtra("state");
        if (!ConnectionStatus.valid(state)) return;
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString("state", state)
                .putLong("received", SystemClock.elapsedRealtime())
                .putInt("boot", bootCount(context)).apply();
        if (version != 2) return; // Old desktops can display state, never auto-play.
        String id = intent.getStringExtra("connection_id");
        SharedPreferences playback = AutoplaySettings.prefs(context);
        // Ordered serialization keeps exactly the most recent IDs across restarts;
        // SharedPreferences string sets do not preserve eviction order.
        Set<String> consumed = new LinkedHashSet<>(Arrays.asList(
                playback.getString("consumed_ids", "").split(",")));
        consumed.remove("");
        if (!AutoplayPolicy.shouldConsume(id, state,
                intent.getBooleanExtra("playback_ready", false),
                intent.getBooleanExtra("play_event", false), consumed.contains(id))) return;
        consumed.add(id);
        while (consumed.size() > 64) consumed.remove(consumed.iterator().next());
        // Persist BEFORE touching a player. No retries on heartbeat, process or
        // listener restart, missing permission/session, disabled setting or failure.
        if (!playback.edit().putString("consumed_ids", String.join(",", consumed)).commit()) return;
        DeskflowNotificationListenerService.playOnConnection(context);
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
