/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

/** Dependency-free regression test; run through tests/test_android_usb_disconnect.py. */
public final class UsbDisconnectMonitorTest {
    private static int checks;

    private static void check(boolean result, String message) {
        checks++;
        if (!result) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        UsbDisconnectMonitor monitor = new UsbDisconnectMonitor();
        check(!monitor.usbState(false), "initial disconnected sticky is not an unplug");
        check(!monitor.usbState(false), "duplicate initial disconnected");
        check(!monitor.usbState(true), "initial connection does not pause");
        check(!monitor.usbState(true), "configured/connected duplicate does not pause");
        check(monitor.usbState(false), "disconnect immediately requests pause in same call");
        check(!monitor.usbState(false), "repeated disconnected does not pause again");
        check(!monitor.accessoryDetached(), "accessory duplicate after USB disconnect ignored");

        // No timer or reconnect cancellation: even rapid re-enumeration pauses.
        for (int i = 0; i < 8; ++i) {
            check(!monitor.usbState(true), "reconnect only re-arms");
            check(monitor.usbState(false), "each immediate unplug/re-enumeration pauses");
            check(!monitor.usbState(false), "duplicate disconnect ignored");
        }

        // Either ordering of both actions results in one immediate pause.
        for (boolean accessoryFirst : new boolean[] {true, false}) {
            monitor.reset();
            check(!monitor.usbState(true), "establish USB connection");
            if (accessoryFirst) {
                check(monitor.accessoryDetached(), "accessory event pauses immediately");
                check(!monitor.usbState(false), "following USB event deduplicated");
            } else {
                check(monitor.usbState(false), "USB event pauses immediately");
                check(!monitor.accessoryDetached(), "following accessory event deduplicated");
            }
            check(!monitor.accessoryDetached(), "repeated accessory event ignored");
        }

        // Without USB_STATE there is no baseline for dedup/reconnect detection.
        monitor.reset();
        check(monitor.accessoryDetached(), "accessory-only fallback immediately pauses");
        check(monitor.accessoryDetached(), "accessory-only subsequent event remains usable");
        check(!monitor.usbState(false), "initial disconnected state does not pause");

        // Service teardown clears the baseline; disconnected rebind never pauses.
        monitor.reset();
        check(!monitor.usbState(true), "connected before service unbind");
        monitor.reset();
        check(!monitor.usbState(false), "rebind while disconnected establishes baseline only");
        check(!monitor.usbState(false), "duplicate disconnected after rebind");
        check(!monitor.usbState(true), "reconnect after rebind");
        check(monitor.usbState(false), "next real disconnect immediately pauses");
        System.out.println("PASS: " + checks + " immediate USB disconnect checks");
    }
}
