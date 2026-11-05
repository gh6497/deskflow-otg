#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute the actual Home/Back accessibility service using host Android fakes.

No SDK, phone or permissions are accessed; ROM key mapping still needs a device.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


STUBS = {
    "android/accessibilityservice/AccessibilityServiceInfo.java": """
package android.accessibilityservice;
public class AccessibilityServiceInfo {
    public static final int FLAG_REQUEST_FILTER_KEY_EVENTS = 32;
    public int flags;
}
""",
    "android/accessibilityservice/AccessibilityService.java": """
package android.accessibilityservice;
import android.view.KeyEvent;
import android.view.accessibility.AccessibilityEvent;
public abstract class AccessibilityService {
    public static final int GLOBAL_ACTION_BACK = 1, GLOBAL_ACTION_HOME = 2;
    public int calls, lastAction, forwarded;
    public boolean succeed = true;
    private AccessibilityServiceInfo info = new AccessibilityServiceInfo();
    public AccessibilityServiceInfo getServiceInfo() { return info; }
    public void setServiceInfo(AccessibilityServiceInfo value) { info = value; }
    protected void onServiceConnected() { }
    public boolean onKeyEvent(KeyEvent event) { ++forwarded; return false; }
    public boolean performGlobalAction(int action) { ++calls; lastAction = action; return succeed; }
    public abstract void onAccessibilityEvent(AccessibilityEvent event);
    public abstract void onInterrupt();
}
""",
    "android/view/KeyEvent.java": """
package android.view;
public final class KeyEvent {
    public static final int KEYCODE_UNKNOWN = 0;
    public static final int KEYCODE_F13 = 326, KEYCODE_F14 = 327;
    public static final int ACTION_DOWN = 0, ACTION_UP = 1;
    private final int code, action, repeat, scan;
    public KeyEvent(int c, int a, int r) { this(c, a, r, 0); }
    public KeyEvent(int c, int a, int r, int s) { code = c; action = a; repeat = r; scan = s; }
    public int getKeyCode() { return code; }
    public int getScanCode() { return scan; }
    public int getAction() { return action; }
    public int getRepeatCount() { return repeat; }
}
""",
    "android/view/accessibility/AccessibilityEvent.java": """
package android.view.accessibility;
public final class AccessibilityEvent { }
""",
    "android/util/Log.java": """
package android.util;
public final class Log {
    public static int i(String tag, String text) { return 0; }
    public static int w(String tag, String text) { return 0; }
}
""",
    "com/github/gh6497/deskflowotg/AccessibilityHostTest.java": """
package com.github.gh6497.deskflowotg;
import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.view.KeyEvent;
public final class AccessibilityHostTest {
    private static void check(boolean result) { if (!result) throw new AssertionError(); }
    public static void main(String[] args) {
        DeskflowAccessibilityService service = new DeskflowAccessibilityService();
        service.onServiceConnected();
        check((service.getServiceInfo().flags & AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS) != 0);
        for (int code : new int[] {KeyEvent.KEYCODE_F13, KeyEvent.KEYCODE_F14}) {
            int before = service.calls;
            check(service.onKeyEvent(new KeyEvent(code, KeyEvent.ACTION_DOWN, 0)));
            check(service.calls == before + 1);
            check(service.lastAction == (code == KeyEvent.KEYCODE_F13 ? AccessibilityService.GLOBAL_ACTION_HOME : AccessibilityService.GLOBAL_ACTION_BACK));
            check(service.onKeyEvent(new KeyEvent(code, KeyEvent.ACTION_DOWN, 1)));
            check(service.onKeyEvent(new KeyEvent(code, KeyEvent.ACTION_UP, 0)));
            check(service.calls == before + 1); // No repeats or action on release.
        }
        // Android 10-15 Generic.kl has no F13/F14 mapping: preserve evdev scan codes.
        for (int scan : new int[] {183, 184}) {
            int before = service.calls;
            check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_DOWN, 0, scan)));
            check(service.calls == before + 1);
            check(service.lastAction == (scan == 183 ? AccessibilityService.GLOBAL_ACTION_HOME : AccessibilityService.GLOBAL_ACTION_BACK));
            check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_DOWN, 1, scan)));
            check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_UP, 0, scan)));
            check(service.calls == before + 1);
        }
        int before = service.calls;
        check(!service.onKeyEvent(new KeyEvent(29, KeyEvent.ACTION_DOWN, 0)));
        check(!service.onKeyEvent(new KeyEvent(29, KeyEvent.ACTION_UP, 0)));
        check(service.calls == before && service.forwarded == 2);
        // Do not treat arbitrary UNKNOWN keys or ROM-remapped known keys as shortcuts.
        for (int scan : new int[] {0, 182, 185}) {
            check(!service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_DOWN, 0, scan)));
            check(!service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_UP, 0, scan)));
        }
        for (int scan : new int[] {183, 184}) {
            check(!service.onKeyEvent(new KeyEvent(29, KeyEvent.ACTION_DOWN, 0, scan)));
            check(!service.onKeyEvent(new KeyEvent(29, KeyEvent.ACTION_UP, 0, scan)));
        }
        check(service.calls == before && service.forwarded == 12);
        service.succeed = false;
        check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_F14, KeyEvent.ACTION_DOWN, 0)));
        check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_F14, KeyEvent.ACTION_UP, 0)));
        check(service.calls == before + 1 && service.forwarded == 12); // Failure never leaks F14.
        check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_DOWN, 0, 184)));
        check(service.onKeyEvent(new KeyEvent(KeyEvent.KEYCODE_UNKNOWN, KeyEvent.ACTION_UP, 0, 184)));
        check(service.calls == before + 2 && service.forwarded == 12);
        service.onInterrupt();
        service.onAccessibilityEvent(null);
        System.out.println("PASS: Android 10-15 scan codes and Android 16 Home/Back, repeats, release, unrelated keys and action failure");
    }
}
""",
}


def main():
    javac, java = shutil.which("javac"), shutil.which("java")
    if not javac or not java:
        print("SKIP: JDK required for Android accessibility host regression")
        return 77
    root = Path(__file__).resolve().parent.parent
    service = root / "android/deskflow-otg/app/src/main/java/com/github/gh6497/deskflowotg/DeskflowAccessibilityService.java"
    with tempfile.TemporaryDirectory(prefix="otg-android-accessibility-") as temporary:
        directory = Path(temporary)
        sources = [str(service)]
        for name, content in STUBS.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            sources.append(str(path))
        subprocess.run([javac, "-encoding", "UTF-8", "-Xlint:all", "-Werror", "-d", temporary, *sources],
                       check=True, timeout=30)
        subprocess.run([java, "-cp", temporary, "com.github.gh6497.deskflowotg.AccessibilityHostTest"],
                       check=True, timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
