/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

public final class AutoplayPolicyTest {
    private static final String ID = "0123456789abcdef0123456789abcdef";

    private static void check(boolean value) {
        if (!value) throw new AssertionError("auto-play policy failed");
    }

    private static final class Player {
        final String name;
        boolean paused = true, supported = true, inspectFails, playFails;
        int attempts;
        Player(String name) { this.name = name; }
    }

    private static final AutoplayPolicy.Actions<Player> ACTIONS = new AutoplayPolicy.Actions<>() {
        public String packageName(Player player) { return player.name; }
        public boolean canResume(Player player) {
            if (player.inspectFails) throw new IllegalStateException("stale session");
            return player.paused && player.supported;
        }
        public void play(Player player) {
            ++player.attempts;
            if (player.playFails) throw new IllegalStateException("player rejected play");
            player.paused = false;
        }
        public void failed(Player player, RuntimeException exception) { }
    };

    public static void main(String[] args) {
        check(AutoplayPolicy.shouldConsume(ID, "connected", true, true, false));
        check(!AutoplayPolicy.shouldConsume(ID, "connected", true, false, false)); // heartbeat
        check(!AutoplayPolicy.shouldConsume(ID, "connected", false, true, false)); // audio not ready
        for (String state : Arrays.asList("connecting", "disconnecting", "disconnected", "unknown"))
            check(!AutoplayPolicy.shouldConsume(ID, state, true, true, false));
        for (String id : Arrays.asList(null, "", "none", "bad id", ID + "0", ID.toUpperCase()))
            check(!AutoplayPolicy.shouldConsume(id, "connected", true, true, false));

        Set<String> persisted = new HashSet<>();
        persisted.add(ID); // Consumed before disabled/missing service/session/failed play.
        Set<String> afterRestart = new HashSet<>(persisted);
        check(!AutoplayPolicy.shouldConsume(ID, "connected", true, true, afterRestart.contains(ID)));
        check(!AutoplayPolicy.shouldConsume(ID, "connected", true, false, false)); // expired lease renews
        check(AutoplayPolicy.shouldConsume("1123456789abcdef0123456789abcdef",
                "connected", true, true, false)); // genuine reconnect

        Player other = new Player("video"), music = new Player("music"), second = new Player("music");
        check(AutoplayPolicy.playOne(Arrays.asList(other, music, second), "music", ACTIONS));
        check(other.attempts == 0 && music.attempts == 1 && second.attempts == 0);
        check(!AutoplayPolicy.playOne(Collections.singletonList(music), "music", ACTIONS)); // playing
        music.paused = true; // manual pause; same connection is still consumed
        check(!AutoplayPolicy.shouldConsume(ID, "connected", true, true, persisted.contains(ID)));
        check(!AutoplayPolicy.playOne(Arrays.asList(other, music), "missing", ACTIONS));
        check(!AutoplayPolicy.playOne(Arrays.asList(other, music), "", ACTIONS));
        check(!AutoplayPolicy.playOne(Collections.emptyList(), "music", ACTIONS));
        music.supported = false;
        check(!AutoplayPolicy.playOne(Collections.singletonList(music), "music", ACTIONS));
        music.inspectFails = true;
        check(AutoplayPolicy.playOne(Arrays.asList(music, second), "music", ACTIONS));
        music.inspectFails = false; music.supported = true; music.playFails = true;
        second.paused = true;
        check(!AutoplayPolicy.playOne(Arrays.asList(music, second), "music", ACTIONS));
        check(music.attempts == 2 && second.attempts == 1); // no second attempt after exception
        System.out.println("PASS: explicit connection edge, dedup/restart and single selected player");
    }
}
