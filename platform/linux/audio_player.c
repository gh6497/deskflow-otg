/* Linux playback for scrcpy's raw audio stream: 4-byte codec id followed by
 * 12-byte big-endian PTS/flags + payload-size headers and S16_LE PCM payloads.
 * Protocol reference: scrcpy demuxer/audio raw stream; ALSA replaces SDL.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "audio_player.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define AUDIO_SAMPLE_RATE 48000U
#define AUDIO_CHANNELS 2U
#define AUDIO_BYTES_PER_SAMPLE 2U
#define AUDIO_FRAME_HEADER_SIZE 12U
#define AUDIO_MAX_FRAME_SIZE (1024U * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define SCRCPY_CODEC_RAW UINT32_C(0x00726177)
#define SCRCPY_CODEC_DISABLED 0U

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)

struct audio_player {
    int socket_fd;
    pthread_t thread;
    atomic_bool ready;
};

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int read_full(int fd, void *buffer, size_t size)
{
    unsigned char *p = buffer;
    while (size) {
        ssize_t r = read(fd, p, size);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)r;
        size -= (size_t)r;
    }
    return 1;
}

static int open_output(snd_pcm_t **pcm)
{
    int error = snd_pcm_open(pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0) return error;
    error = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               AUDIO_CHANNELS, AUDIO_SAMPLE_RATE, 1, 100000);
    if (error < 0) {
        snd_pcm_close(*pcm);
        *pcm = NULL;
    }
    return error;
}

static int play_pcm(snd_pcm_t *pcm, const unsigned char *data, size_t size)
{
    size_t frames = size / (AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE);
    const int16_t *samples = (const int16_t *)data;
    while (frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, samples, frames);
        if (written == -EPIPE) {
            if (snd_pcm_prepare(pcm) < 0) return -1;
            continue;
        }
        if (written < 0) {
            written = snd_pcm_recover(pcm, (int)written, 1);
            if (written < 0) return -1;
            continue;
        }
        samples += (size_t)written * AUDIO_CHANNELS;
        frames -= (size_t)written;
    }
    return 0;
}

static void *audio_thread_main(void *userdata)
{
    struct audio_player *player = userdata;
    unsigned char codec_buffer[4];
    unsigned char header[AUDIO_FRAME_HEADER_SIZE];
    unsigned char *payload = NULL;
    snd_pcm_t *pcm = NULL;

    if (read_full(player->socket_fd, codec_buffer, sizeof(codec_buffer)) != 1) {
        LOG_WARN("scrcpy audio stream closed before its header\n");
        goto done;
    }
    if (read_be32(codec_buffer) == SCRCPY_CODEC_DISABLED) {
        LOG_WARN("Android could not capture audio; continuing without audio\n");
        goto done;
    }
    if (read_be32(codec_buffer) != SCRCPY_CODEC_RAW) {
        LOG_ERR("scrcpy audio codec is not raw PCM; use audio_codec=raw\n");
        goto done;
    }
    if (open_output(&pcm) < 0) {
        LOG_ERR("could not open ALSA playback device\n");
        goto done;
    }
    atomic_store(&player->ready, true);
    while (read_full(player->socket_fd, header, sizeof(header)) == 1) {
        /* PTS/flags are consumed with the header; RAW playback uses no clock. */
        uint32_t size = read_be32(header + 8);
        if (!size || size > AUDIO_MAX_FRAME_SIZE) {
            LOG_ERR("invalid scrcpy audio frame size: %u\n", size);
            break;
        }
        unsigned char *new_payload = realloc(payload, size);
        if (!new_payload) break;
        payload = new_payload;
        if (read_full(player->socket_fd, payload, size) != 1) break;
        if (play_pcm(pcm, payload, size) < 0) break;
    }
done:
    atomic_store(&player->ready, false);
    free(payload);
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    return NULL;
}

struct audio_player *audio_player_start(intptr_t socket_fd)
{
    struct audio_player *player = malloc(sizeof(*player));
    if (!player) return NULL;
    player->socket_fd = (int)socket_fd;
    atomic_init(&player->ready, false);
    if (pthread_create(&player->thread, NULL, audio_thread_main, player) != 0) {
        free(player);
        return NULL;
    }
    return player;
}

bool audio_player_ready(struct audio_player *player)
{
    return player && atomic_load(&player->ready);
}

void audio_player_stop(struct audio_player *player)
{
    if (!player) return;
    (void)shutdown(player->socket_fd, SHUT_RDWR);
    (void)pthread_join(player->thread, NULL);
    free(player);
}
