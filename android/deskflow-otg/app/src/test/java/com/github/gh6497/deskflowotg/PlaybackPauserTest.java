/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import java.util.Arrays;
import java.util.Collections;

public final class PlaybackPauserTest {
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static final class Player {
        final boolean playing;
        boolean readFailure;
        boolean pauseFailure;
        int attempts;
        int failures;

        Player(boolean playing) { this.playing = playing; }
    }

    private static final PlaybackPauser.Actions<Player> ACTIONS = new PlaybackPauser.Actions<Player>() {
        @Override
        public boolean isPlaying(Player player) {
            if (player.readFailure) throw new IllegalStateException("stale session");
            return player.playing;
        }

        @Override
        public void pause(Player player) {
            player.attempts++;
            if (player.pauseFailure) throw new SecurityException("player rejected pause");
        }

        @Override
        public void failed(Player player, RuntimeException exception) { player.failures++; }
    };

    public static void main(String[] args) {
        Player first = new Player(true);
        Player second = new Player(true);
        Player paused = new Player(false);
        Player buffering = new Player(true);
        check(PlaybackPauser.pauseAll(Arrays.asList(first, paused, second, buffering), ACTIONS) == 3,
                "all eligible sessions, not just first");
        check(first.attempts == 1 && second.attempts == 1 && buffering.attempts == 1,
                "exactly one pause per eligible session");
        check(paused.attempts == 0, "paused session left unchanged");

        Player rejected = new Player(true);
        rejected.pauseFailure = true;
        Player stale = new Player(true);
        stale.readFailure = true;
        Player survivor = new Player(true);
        check(PlaybackPauser.pauseAll(Arrays.asList(rejected, stale, paused, survivor), ACTIONS) == 1,
                "state/pause failures do not block later players");
        check(rejected.attempts == 1 && rejected.failures == 1, "pause failure recorded once");
        check(stale.attempts == 0 && stale.failures == 1, "state failure recorded once");
        check(survivor.attempts == 1, "later session still paused");
        check(PlaybackPauser.pauseAll(Collections.<Player>emptyList(), ACTIONS) == 0, "empty list");
        check(PlaybackPauser.pauseAll(Arrays.asList(paused), ACTIONS) == 0, "no playing sessions");
        check(paused.attempts == 0, "never send play-pause to inactive session");
        System.out.println("PASS: " + checks + " multi-session pause checks");
    }
}
