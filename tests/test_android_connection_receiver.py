#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the real receiver/settings on a tiny host Android API fake, no phone/SDK.

This tests dispatch and persistent consumption, not Android permission enforcement
or the real notification-listener binding (those still need device validation).
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


STUBS = {
    "android/content/BroadcastReceiver.java": """
package android.content;
public abstract class BroadcastReceiver {
    public abstract void onReceive(Context context, Intent intent);
}
""",
    "android/content/Context.java": """
package android.content;
import java.util.HashMap;
import java.util.Map;
public class Context {
    public static final int MODE_PRIVATE = 0;
    private final Map<String, SharedPreferences> stores = new HashMap<>();
    public SharedPreferences getSharedPreferences(String name, int mode) {
        return stores.computeIfAbsent(name, key -> new SharedPreferences());
    }
    public Object getContentResolver() { return this; }
}
""",
    "android/content/SharedPreferences.java": """
package android.content;
import java.util.HashMap;
import java.util.Map;
public class SharedPreferences {
    private final Map<String, Object> values = new HashMap<>();
    public boolean writable = true;
    public String getString(String k, String v) { return (String) values.getOrDefault(k, v); }
    public long getLong(String k, long v) { return (Long) values.getOrDefault(k, v); }
    public int getInt(String k, int v) { return (Integer) values.getOrDefault(k, v); }
    public boolean getBoolean(String k, boolean v) { return (Boolean) values.getOrDefault(k, v); }
    public Editor edit() { return new Editor(); }
    public final class Editor {
        private final Map<String, Object> staged = new HashMap<>();
        public Editor putString(String k, String v) { staged.put(k, v); return this; }
        public Editor putLong(String k, long v) { staged.put(k, v); return this; }
        public Editor putInt(String k, int v) { staged.put(k, v); return this; }
        public Editor putBoolean(String k, boolean v) { staged.put(k, v); return this; }
        public void apply() { values.putAll(staged); }
        public boolean commit() { if (!writable) return false; apply(); return true; }
    }
}
""",
    "android/content/Intent.java": """
package android.content;
import java.util.HashMap;
import java.util.Map;
public class Intent {
    private final Map<String, Object> extras = new HashMap<>();
    public String getAction() { return "com.github.gh6497.deskflowotg.CONNECTION_STATUS"; }
    public Intent put(String k, Object v) { extras.put(k, v); return this; }
    public String getStringExtra(String k) { return (String) extras.get(k); }
    public int getIntExtra(String k, int v) { return (Integer) extras.getOrDefault(k, v); }
    public boolean getBooleanExtra(String k, boolean v) { return (Boolean) extras.getOrDefault(k, v); }
}
""",
    "android/os/SystemClock.java": """
package android.os;
public final class SystemClock {
    public static long now = 1000;
    public static long elapsedRealtime() { return now; }
}
""",
    "android/provider/Settings.java": """
package android.provider;
public final class Settings {
    public static final class Global {
        public static final String BOOT_COUNT = "boot_count";
        public static int boot = 1;
        public static int getInt(Object resolver, String key, int fallback) { return boot; }
    }
}
""",
    "com/github/gh6497/deskflowotg/DeskflowNotificationListenerService.java": """
package com.github.gh6497.deskflowotg;
import android.content.Context;
final class DeskflowNotificationListenerService {
    static boolean bound = true;
    static int dispatched, played;
    static void playOnConnection(Context context) {
        ++dispatched;
        String history = AutoplaySettings.prefs(context).getString("consumed_ids", "");
        if (history.isEmpty()) throw new AssertionError("must persist before media side effect");
        if (bound && AutoplaySettings.enabled(context) && !AutoplaySettings.target(context).isEmpty())
            ++played;
    }
}
""",
    "com/github/gh6497/deskflowotg/ReceiverHostTest.java": """
package com.github.gh6497.deskflowotg;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.SystemClock;
import android.provider.Settings;
public final class ReceiverHostTest {
    private static String id(int n) { return String.format("%032x", n); }
    private static Intent event(int n, boolean edge, boolean ready, String state) {
        return new Intent().put("version", 2).put("connection_id", id(n))
                .put("state", state).put("play_event", edge).put("playback_ready", ready);
    }
    private static void check(boolean result) { if (!result) throw new AssertionError(); }
    public static void main(String[] args) {
        Context context = new Context();
        ConnectionStatusReceiver receiver = new ConnectionStatusReceiver();
        SharedPreferences prefs = AutoplaySettings.prefs(context);
        check(!AutoplaySettings.enabled(context));
        prefs.edit().putString("target", "music").apply();
        receiver.onReceive(context, event(1, true, true, "connected")); // off by default
        check(DeskflowNotificationListenerService.played == 0);
        prefs.edit().putBoolean("enabled", true).apply();
        receiver.onReceive(context, event(1, true, true, "connected")); // no retroactive enable
        receiver.onReceive(context, event(1, false, true, "connected")); // heartbeat
        check(DeskflowNotificationListenerService.dispatched == 1);
        receiver.onReceive(context, event(2, true, false, "connected")); // wait for audio
        check(DeskflowNotificationListenerService.dispatched == 1);
        receiver.onReceive(context, event(2, true, true, "connected"));
        check(DeskflowNotificationListenerService.played == 1);
        receiver = new ConnectionStatusReceiver(); // object/process state is not the dedup source
        receiver.onReceive(context, event(2, true, true, "connected"));
        check(DeskflowNotificationListenerService.played == 1);
        DeskflowNotificationListenerService.bound = false;
        receiver.onReceive(context, event(3, true, true, "connected"));
        DeskflowNotificationListenerService.bound = true;
        receiver.onReceive(context, event(3, true, true, "connected")); // rebind cannot retry
        check(DeskflowNotificationListenerService.played == 1);
        SystemClock.now += 15000;
        check("unknown".equals(ConnectionStatusReceiver.current(context)));
        receiver.onReceive(context, event(3, false, true, "connected")); // expired lease renewal
        Settings.Global.boot++;
        receiver.onReceive(context, event(3, false, true, "connected")); // reboot renewal
        check(DeskflowNotificationListenerService.played == 1);
        receiver.onReceive(context, event(4, true, true, "disconnecting"));
        receiver.onReceive(context, event(4, true, true, "connected").put("version", 1));
        receiver.onReceive(context, event(4, true, true, "connected").put("version", 9));
        receiver.onReceive(context, event(4, true, true, "connected").put("connection_id", "bad"));
        check(DeskflowNotificationListenerService.played == 1);
        prefs.writable = false;
        receiver.onReceive(context, event(4, true, true, "connected")); // persistence failure
        check(DeskflowNotificationListenerService.played == 1);
        prefs.writable = true;
        receiver.onReceive(context, event(4, false, true, "connected")); // no retry on heartbeat
        for (int n = 5; n <= 80; ++n) receiver.onReceive(context, event(n, true, true, "connected"));
        String[] history = prefs.getString("consumed_ids", "").split(",");
        check(history.length == 64 && history[0].equals(id(17)) && history[63].equals(id(80)));
        int played = DeskflowNotificationListenerService.played;
        receiver.onReceive(context, event(80, true, true, "connected"));
        check(DeskflowNotificationListenerService.played == played);
        System.out.println("PASS: real receiver versioning, opt-in, durable edge consumption and bounded history");
    }
}
""",
}


def main():
    javac, java = shutil.which("javac"), shutil.which("java")
    if not javac or not java:
        print("SKIP: JDK required for Android receiver host regression")
        return 77
    root = Path(__file__).resolve().parent.parent
    app = root / "android/deskflow-otg/app/src/main"
    manifest = ET.parse(app / "AndroidManifest.xml")
    android = "{http://schemas.android.com/apk/res/android}"
    receiver = manifest.find("application/receiver")
    assert receiver.get(android + "permission") == "android.permission.DUMP"
    assert receiver.get(android + "exported") == "true"
    with tempfile.TemporaryDirectory(prefix="otg-android-receiver-") as temporary:
        directory = Path(temporary)
        sources = []
        for name, content in STUBS.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            sources.append(str(path))
        package = app / "java/com/github/gh6497/deskflowotg"
        sources += [str(package / name) for name in (
            "ConnectionStatusReceiver.java", "ConnectionStatus.java", "AutoplayPolicy.java", "AutoplaySettings.java")]
        subprocess.run([javac, "-encoding", "UTF-8", "-Xlint:all", "-Werror", "-d", temporary, *sources],
                       check=True, timeout=30)
        subprocess.run([java, "-cp", temporary, "com.github.gh6497.deskflowotg.ReceiverHostTest"],
                       check=True, timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
