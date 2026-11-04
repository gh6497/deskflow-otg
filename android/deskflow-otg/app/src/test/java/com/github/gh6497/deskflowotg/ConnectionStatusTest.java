/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

public final class ConnectionStatusTest {
    private static void expect(String expected, String state, long received, int boot,
                               long now, int currentBoot) {
        String actual = ConnectionStatus.current(state, received, boot, now, currentBoot);
        if (!expected.equals(actual)) throw new AssertionError(expected + " != " + actual);
    }

    public static void main(String[] args) {
        for (String state : new String[]{"connecting", "connected", "disconnecting", "disconnected"}) {
            expect(state, state, 1000, 42, 1000, 42);
            expect(state, state, 1000, 42, 15999, 42);
            expect("unknown", state, 1000, 42, 16000, 42);
            expect("unknown", state, 1000, 42, 2000, 43);
            expect("unknown", state, 1000, 42, 999, 42);
            expect("unknown", state, -1, 42, 1000, 42);
            expect("unknown", state, 1000, -1, 1000, -1);
        }
        expect("unknown", null, 1000, 42, 1000, 42);
        expect("unknown", "usb-connected", 1000, 42, 1000, 42);
        expect("unknown", "CONNECTED", 1000, 42, 1000, 42);
        expect("connected", "connected", 15000, 42, 16000, 42); // heartbeat renews lease
        expect("disconnected", "disconnected", 16000, 42, 16001, 42);
        System.out.println("PASS: Deskflow status validation, heartbeat expiry and reboot policy");
    }
}
