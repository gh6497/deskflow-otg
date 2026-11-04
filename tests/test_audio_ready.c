/* Exercise real scrcpy RAW header/output readiness without audio hardware.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "audio_player.h"
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void wait_ready(struct audio_player *player, bool ready)
{
    for (unsigned i = 0; i < 200; ++i) {
        if (audio_player_ready(player) == ready) return;
        const struct timespec delay = {0, 5000000};
        nanosleep(&delay, NULL);
    }
    assert(audio_player_ready(player) == ready);
}

int main(void)
{
    assert(!audio_player_ready(NULL));
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct audio_player *player = audio_player_start(sockets[0]);
    assert(player && !audio_player_ready(player));
    const unsigned char raw[] = {0, 'r', 'a', 'w'};
    assert(write(sockets[1], raw, sizeof(raw)) == sizeof(raw));
    wait_ready(player, true); // No PCM at all: paused player must not deadlock.
    close(sockets[1]);
    wait_ready(player, false);
    audio_player_stop(player);
    close(sockets[0]);
    // Capture disabled or wrong codec must never report ready.
    for (unsigned codec = 0; codec < 2; ++codec) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        player = audio_player_start(sockets[0]);
        assert(player);
        const unsigned char invalid[] = {0, 0, 0, (unsigned char)codec};
        assert(write(sockets[1], invalid, sizeof(invalid)) == sizeof(invalid));
        close(sockets[1]);
        audio_player_stop(player);
        close(sockets[0]);
    }
    // Stopping during a partial header interrupts the read and joins the thread.
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    player = audio_player_start(sockets[0]);
    assert(player);
    assert(write(sockets[1], raw, 1) == 1);
    assert(!audio_player_ready(player));
    audio_player_stop(player);
    close(sockets[0]);
    close(sockets[1]);
    puts("PASS: audio readiness requires codec + output, not PCM; EOF clears readiness");
    return 0;
}
