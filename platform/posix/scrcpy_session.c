/*
 * scrcpy_session.c - POSIX ADB/server lifetime and scrcpy stream setup.
 * Protocol reference: scrcpy server DesktopConnection (audio/control order).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "scrcpy_session.h"
#include "adb_target.h"
#if defined(OTG_HAVE_AUDIO) || defined(OTG_HAVE_CLIPBOARD)
#ifdef OTG_HAVE_AUDIO
#include "audio_player.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SCRCPY_REMOTE_PATH "/data/local/tmp/deskflow-otg-scrcpy-server.jar"

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)

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

static uint64_t auxiliary_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int wait_child(pid_t pid)
{
    int status;
    uint64_t deadline = auxiliary_now_ms() + 5000;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) return -1;
        if (auxiliary_now_ms() >= deadline) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        (void)poll(NULL, 0, 10);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int adb_target_command(void *ud, const char *target, bool connect)
{
    const char *adb = ud;
    int output[2] = {-1, -1};
    pid_t pid;
    if (!connect) {
        if (pipe(output) < 0) return -1;
        /* Never block on output left open by a descendant after child exit. */
        if (fcntl(output[0], F_SETFL, O_NONBLOCK) < 0) {
            close(output[0]); close(output[1]);
            return -1;
        }
    }
    pid = fork();
    if (pid == 0) {
        if (connect) {
            /* Keep human-readable ADB output out of GUI's JSON stdout. */
            if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(127);
            execlp(adb, adb, "connect", target, (char *)NULL);
        } else {
            int null_fd = open("/dev/null", O_WRONLY);
            close(output[0]);
            if (dup2(output[1], STDOUT_FILENO) < 0 || null_fd < 0 ||
                dup2(null_fd, STDERR_FILENO) < 0) _exit(127);
            if (output[1] > STDERR_FILENO) close(output[1]);
            if (null_fd > STDERR_FILENO) close(null_fd);
            execlp(adb, adb, "-s", target, "get-state", (char *)NULL);
        }
        _exit(127);
    }
    if (!connect) close(output[1]);
    int result = pid < 0 ? -1 : wait_child(pid);
    if (!connect) {
        char state[32];
        ssize_t size;
        do { size = read(output[0], state, sizeof(state)); } while (size < 0 && errno == EINTR);
        close(output[0]);
        if (size < 0 || !otg_adb_state_is_device(state, (size_t)size)) result = -1;
    }
    return result;
}

static int adb_push(const char *adb, const char *serial, const char *source)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp(adb, adb, "-s", serial, "push", source,
              SCRCPY_REMOTE_PATH, (char *)NULL);
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
        execlp(adb, adb, "-s", serial, "forward", local, remote,
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
        execlp(adb, adb, "-s", serial, "forward", "--remove", local,
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
    for (int attempt = 0; attempt < 50; ++attempt) {
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

static int start_server_process(struct scrcpy_session *session, unsigned scid,
                                const char *server_version, bool audio, bool clipboard)
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
        execlp(session->adb_path, session->adb_path, "-s", session->adb_serial,
              "shell", "CLASSPATH=" SCRCPY_REMOTE_PATH, "app_process", "/",
              "com.genymobile.scrcpy.Server", server_version, audio ? "audio=true" : "audio=false", "video=false",
              clipboard ? "control=true" : "control=false", "clipboard_autosync=true",
              "cleanup=true", "tunnel_forward=true",
              "send_device_meta=false", "send_dummy_byte=true",
              "send_stream_meta=true",
              "send_frame_meta=true", "audio_codec=raw",
              "audio_source=output", scid_arg, (char *)NULL);
        _exit(127);
    }
    session->server_pid = (int)pid;
    return 0;
}

static void cleanup_process(struct scrcpy_session *session)
{
    char port_arg[32];
    if (session->server_pid > 0) {
        (void)kill((pid_t)session->server_pid, SIGTERM);
        (void)wait_child((pid_t)session->server_pid);
        session->server_pid = 0;
    }
    if (session->forward_port > 0) {
        snprintf(port_arg, sizeof(port_arg), "tcp:%d", session->forward_port);
        (void)adb_remove_forward(session->adb_path, session->adb_serial, port_arg);
        session->forward_port = 0;
    }
}

int scrcpy_session_start(struct scrcpy_session *session, const char *adb_path,
                         const char *adb_serial, const char *server_path,
                         const char *server_version, bool audio, bool clipboard)
{
#ifndef OTG_HAVE_AUDIO
    if (audio) return -1;
#endif
    unsigned scid;
    char remote[128];
    char local[32];
    int port;
    if (!session) return -1;
    memset(session, 0, sizeof(*session));
    session->initialized = true;
    session->audio_fd = -1;
    session->control_fd = -1;
    if (!adb_path || !adb_serial || !server_path || !server_version || !*adb_serial ||
        !*server_path || !*server_version || (!audio && !clipboard)) {
        LOG_ERR("scrcpy session requires ADB path, ADB serial, server/version and an enabled feature\n");
        return -1;
    }
    session->adb_path = copy_string(adb_path);
    session->adb_serial = copy_string(adb_serial);
    session->server_path = copy_string(server_path);
    if (!session->adb_path || !session->adb_serial || !session->server_path) goto fail;
    if (otg_adb_target_prepare(session->adb_serial, adb_target_command, session->adb_path)) goto fail;
    if (adb_push(session->adb_path, session->adb_serial, session->server_path) != 0) {
        LOG_ERR("adb push of scrcpy-server failed\n");
        goto fail;
    }
    port = choose_port();
    if (port < 0) goto fail;
    scid = (unsigned)getpid() & 0x7fffffffU;
    snprintf(remote, sizeof(remote), "localabstract:scrcpy_%08x", scid);
    snprintf(local, sizeof(local), "tcp:%d", port);
    if (start_server_process(session, scid, server_version, audio, clipboard) != 0) goto fail;
    session->forward_port = port;
    if (adb_forward(session->adb_path, session->adb_serial, local, remote) != 0) {
        LOG_ERR("adb forward to %s failed\n", remote);
        goto fail;
    }
    uint64_t deadline = auxiliary_now_ms() + 5000;
    while (auxiliary_now_ms() < deadline) {
        session->audio_fd = connect_local(port);
        if (session->audio_fd >= 0) {
            if (wait_for_stream((int)session->audio_fd) == 0) break;
            close((int)session->audio_fd);
            session->audio_fd = -1;
        }
        (void)poll(NULL, 0, 100);
    }
    if (session->audio_fd < 0) {
        LOG_ERR("scrcpy-server did not open its auxiliary stream\n");
        goto fail;
    }
    /* The first socket's dummy byte is sent BEFORE the server accepts the
     * control socket. Waiting for audio metadata here would deadlock. */
    unsigned char dummy;
    if (read_full((int)session->audio_fd, &dummy, 1) != 1 || dummy != 0) goto fail;
    if (clipboard) {
        if (audio) session->control_fd = connect_local(port);
        else { session->control_fd = session->audio_fd; session->audio_fd = -1; }
        if (session->control_fd < 0 ||
            otg_clipboard_channel_open(&session->clipboard, session->control_fd)) goto fail;
    }
#ifdef OTG_HAVE_AUDIO
    if (audio) {
        session->player = audio_player_start(session->audio_fd);
        if (!session->player) goto fail;
    }
#endif
    session->started = true;
    LOG_INFO("scrcpy auxiliary session started (audio=%s, clipboard=%s)\n",
             audio ? "on" : "off", clipboard ? "on" : "off");
    return 0;
fail:
    scrcpy_session_stop(session);
    return -1;
}

void scrcpy_session_stop(struct scrcpy_session *session)
{
    if (!session || !session->initialized) return;
    otg_clipboard_channel_close(&session->clipboard);
    if (session->control_fd >= 0) {
        shutdown((int)session->control_fd, SHUT_RDWR);
        close((int)session->control_fd);
        session->control_fd = -1;
    }
#ifdef OTG_HAVE_AUDIO
    audio_player_stop(session->player);
#endif
    if (session->audio_fd >= 0) close((int)session->audio_fd);
    cleanup_process(session);
    free(session->adb_path);
    free(session->adb_serial);
    free(session->server_path);
    memset(session, 0, sizeof(*session));
    session->audio_fd = session->control_fd = -1;
}

bool scrcpy_session_started(const struct scrcpy_session *session)
{
    return session && session->started;
}

#endif
