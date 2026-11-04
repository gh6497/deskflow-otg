/* scrcpy raw audio playback, independent of ADB and clipboard state.
 * Protocol reference: scrcpy audio stream (codec id + framed raw PCM).
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DESKFLOW_OTG_AUDIO_PLAYER_H
#define DESKFLOW_OTG_AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

struct audio_player;

/* Borrow a connected socket positioned after the scrcpy dummy byte. The owner
 * must keep it open until stop returns. Playback failures remain nonfatal to
 * the shared session. Available only when OTG_HAVE_AUDIO is enabled. */
struct audio_player *audio_player_start(intptr_t socket_fd);

/* Thread-safe snapshot: valid codec header and platform output opened. This
 * does not wait for PCM (a paused phone may produce no frames). NULL is false. */
bool audio_player_ready(struct audio_player *player);

/* Interrupt socket reads, join the playback thread and free the player.
 * Does not close the borrowed socket. NULL is allowed. */
void audio_player_stop(struct audio_player *player);

#endif /* DESKFLOW_OTG_AUDIO_PLAYER_H */
