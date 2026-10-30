/*
 * audio_session.c - scrcpy-compatible Android audio forwarding.
 *
 * The wire format is intentionally the current scrcpy stream format:
 *   64-byte device metadata, 4-byte big-endian codec id, then 12-byte
 *   big-endian PTS/flags + payload-size frame headers.  The Android side is
 * started with audio_codec=raw, so the payload is signed little-endian PCM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_session.h"

#ifdef OTG_HAVE_AUDIO

#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define AUDIO_SAMPLE_RATE 48000U
#define AUDIO_CHANNELS 2U
#define AUDIO_BYTES_PER_SAMPLE 2U
#define AUDIO_DEVICE_META_SIZE 64U
#define AUDIO_FRAME_HEADER_SIZE 12U
#define AUDIO_MAX_FRAME_SIZE (1024U * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_REMOTE_PATH "/data/local/tmp/deskflow-otg-scrcpy-server.jar"
#define SCRCPY_CODEC_RAW UINT32_C(0x00726177)
#define SCRCPY_CODEC_DISABLED 0U

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)

struct audio_thread_args {
    struct audio_session *session;
};

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const unsigned char *p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
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

static char *copy_string(const char *value)
{
    size_t length;
    char *copy;
    if (!value) return NULL;
    length = strlen(value);
    copy = malloc(length + 1);
    if (copy) memcpy(copy, value, length + 1);
    return copy;
}

static int wait_child(pid_t pid)
{
    int status;
    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) break;
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int adb_push(const char *adb, const char *serial, const char *source)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(adb, adb, "-s", serial, "push", source,
              AUDIO_REMOTE_PATH, (char *)NULL);
        _exit(127);
    }
    return wait_child(pid);
}

static int adb_forward(const char *adb, const char *serial,
                       const char *local, const char *remote)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(adb, adb, "-s", serial, "forward", local, remote,
              (char *)NULL);
        _exit(127);
    }
    return wait_child(pid);
}

static int adb_remove_forward(const char *adb, const char *serial,
                              const char *local)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(adb, adb, "-s", serial, "forward", "--remove", local,
              (char *)NULL);
        _exit(127);
    }
    return wait_child(pid);
}

static int choose_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int port;
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) < 0) {
        close(fd);
        return -1;
    }
    port = ntohs(address.sin_port);
    close(fd);
    return port;
}

static int connect_local(int port)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ADB may accept the host-side TCP connection before adbd has connected to
 * the server's abstract socket.  Do not hand such an EOF to the audio thread:
 * wait until at least one byte of the scrcpy stream is available. */
static int wait_for_stream(int fd)
{
    struct pollfd descriptor = { .fd = fd, .events = POLLIN };
    unsigned char byte;
    for (int attempt = 0; attempt < 100; ++attempt) {
        int result = poll(&descriptor, 1, 100);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!result) continue;
        if (descriptor.revents & (POLLERR | POLLNVAL)) return -1;
        if (recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 1) return 0;
        if (descriptor.revents & POLLHUP) return -1;
    }
    return -1;
}

static int start_server_process(struct audio_session *session, unsigned scid,
                                const char *server_version)
{
    char scid_arg[32];
    pid_t pid;
    snprintf(scid_arg, sizeof(scid_arg), "scid=%08x", scid);
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        execl(session->adb_path, session->adb_path, "-s", session->serial,
              "shell", "CLASSPATH=" AUDIO_REMOTE_PATH, "app_process", "/",
              "com.genymobile.scrcpy.Server", server_version, "audio=true", "video=false",
              "control=false", "cleanup=true", "tunnel_forward=true",
              "send_device_meta=true", "send_dummy_byte=false",
              "send_stream_meta=true",
              "send_frame_meta=true", "audio_codec=raw",
              "audio_source=output", scid_arg, (char *)NULL);
        _exit(127);
    }
    session->server_pid = (int)pid;
    return 0;
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
    struct audio_thread_args *args = userdata;
    struct audio_session *session = args->session;
    unsigned char meta[AUDIO_DEVICE_META_SIZE];
    unsigned char codec_buffer[4];
    unsigned char header[AUDIO_FRAME_HEADER_SIZE];
    unsigned char *payload = NULL;
    snd_pcm_t *pcm = NULL;
    int result;
    free(args);

    result = read_full(session->socket_fd, meta, sizeof(meta));
    if (result != 1 || read_full(session->socket_fd, codec_buffer, 4) != 1) {
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
    session->pcm = pcm;
    while (!session->stop_requested) {
        uint32_t size;
        result = read_full(session->socket_fd, header, sizeof(header));
        if (result != 1) break;
        (void)read_be64(header); /* PTS and flags are consumed by the sink. */
        size = read_be32(header + 8);
        if (!size || size > AUDIO_MAX_FRAME_SIZE) {
            LOG_ERR("invalid scrcpy audio frame size: %u\n", size);
            break;
        }
        {
            unsigned char *new_payload = realloc(payload, size);
            if (!new_payload) break;
            payload = new_payload;
        }
        if (read_full(session->socket_fd, payload, size) != 1) break;
        if (play_pcm(pcm, payload, size) < 0) break;
    }
    free(payload);
done:
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    session->pcm = NULL;
    return NULL;
}

static void cleanup_process(struct audio_session *session)
{
    char port_arg[32];
    if (session->server_pid > 0) {
        (void)kill((pid_t)session->server_pid, SIGTERM);
        (void)waitpid((pid_t)session->server_pid, NULL, 0);
        session->server_pid = 0;
    }
    if (session->forward_port > 0) {
        snprintf(port_arg, sizeof(port_arg), "tcp:%d", session->forward_port);
        (void)adb_remove_forward(session->adb_path, session->serial, port_arg);
        session->forward_port = 0;
    }
}

int audio_session_start(struct audio_session *session, const char *adb_path,
                        const char *serial, const char *server_path,
                        const char *server_version)
{
    struct audio_thread_args *args;
    pthread_t *thread;
    unsigned scid;
    char remote[128];
    char local[32];
    int port;
    memset(session, 0, sizeof(*session));
    session->socket_fd = -1;
    if (!adb_path || !serial || !server_path || !server_version || !*serial ||
        !*server_path || !*server_version) {
        LOG_ERR("audio requires --adb, --serial, --scrcpy-server and --scrcpy-version\n");
        return -1;
    }
    session->adb_path = copy_string(adb_path);
    session->serial = copy_string(serial);
    session->server_path = copy_string(server_path);
    if (!session->adb_path || !session->serial || !session->server_path) goto fail;
    if (adb_push(session->adb_path, session->serial, session->server_path) != 0) {
        LOG_ERR("adb push of scrcpy-server failed\n");
        goto fail;
    }
    port = choose_port();
    if (port < 0) goto fail;
    scid = (unsigned)getpid() & 0x7fffffffU;
    snprintf(remote, sizeof(remote), "localabstract:scrcpy_%08x", scid);
    snprintf(local, sizeof(local), "tcp:%d", port);
    if (start_server_process(session, scid, server_version) != 0) goto fail;
    session->forward_port = port;
    if (adb_forward(session->adb_path, session->serial, local, remote) != 0) {
        LOG_ERR("adb forward to %s failed\n", remote);
        goto fail;
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        session->socket_fd = connect_local(port);
        if (session->socket_fd >= 0) {
            if (wait_for_stream(session->socket_fd) == 0) break;
            close(session->socket_fd);
            session->socket_fd = -1;
        }
        (void)poll(NULL, 0, 100);
    }
    if (session->socket_fd < 0) {
        LOG_ERR("scrcpy-server did not produce an audio stream\n");
        goto fail;
    }
    args = malloc(sizeof(*args));
    if (!args) goto fail;
    args->session = session;
    thread = malloc(sizeof(*thread));
    if (!thread || pthread_create(thread, NULL, audio_thread_main, args) != 0) {
        free(thread);
        free(args);
        goto fail;
    }
    session->thread = thread;
    session->started = true;
    LOG_INFO("scrcpy audio forwarding started (raw PCM, 48 kHz stereo)\n");
    return 0;
fail:
    audio_session_stop(session);
    return -1;
}

void audio_session_stop(struct audio_session *session)
{
    pthread_t *thread;
    if (!session) return;
    session->stop_requested = true;
    if (session->socket_fd >= 0) {
        shutdown(session->socket_fd, SHUT_RDWR);
        close(session->socket_fd);
        session->socket_fd = -1;
    }
    thread = session->thread;
    if (session->started) {
        (void)pthread_join(*thread, NULL);
        free(thread);
        session->started = false;
    }
    cleanup_process(session);
    free(session->adb_path);
    free(session->serial);
    free(session->server_path);
    memset(session, 0, sizeof(*session));
    session->socket_fd = -1;
}

bool audio_session_started(const struct audio_session *session)
{
    return session && session->started;
}

#else

#include <stdio.h>
#include <string.h>

int audio_session_start(struct audio_session *session, const char *adb_path,
                        const char *serial, const char *server_path,
                        const char *server_version)
{
    (void)session;
    (void)adb_path;
    (void)serial;
    (void)server_path;
    (void)server_version;
    fprintf(stderr, "ERROR: audio forwarding is not enabled in this build\n");
    return -1;
}

void audio_session_stop(struct audio_session *session)
{
    if (session) memset(session, 0, sizeof(*session));
}

bool audio_session_started(const struct audio_session *session)
{
    return session && session->started;
}

#endif
