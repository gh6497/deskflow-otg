/* Windows ADB/server lifetime and scrcpy stream setup.
 * Protocol reference: scrcpy server DesktopConnection (audio/control order).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "scrcpy_session.h"
#include "adb_target.h"

#if defined(OTG_HAVE_AUDIO) || defined(OTG_HAVE_CLIPBOARD)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef OTG_HAVE_AUDIO
#include "audio_player.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REMOTE_PATH "/data/local/tmp/deskflow-otg-scrcpy-server.jar"

#define LOG_ERR(...) fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)

static int read_full(SOCKET s, void *out, size_t n) {
    unsigned char *p = out;
    while (n) {
        int got = recv(s, (char *)p, (int)n, 0);
        if (got <= 0) return got == 0 ? 0 : -1;
        p += got; n -= (size_t)got;
    }
    return 1;
}

static int run_adb_timeout(const char *adb, const char *serial, const char *command_line,
                           DWORD timeout_ms) {
    char command[2048];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    snprintf(command, sizeof(command), "\"%s\" -s \"%s\" %s",
             adb, serial, command_line);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) return -1;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        (void)WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return -1;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0 ? 0 : -1;
}

static int run_adb(const char *adb, const char *serial, const char *command_line) {
    return run_adb_timeout(adb, serial, command_line, 5000);
}

static int adb_target_command(void *ud, const char *target, bool connect) {
    const char *adb = ud;
    char command[2048], output[512];
    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    HANDLE reader = NULL, writer = NULL, null_input = INVALID_HANDLE_VALUE;
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    DWORD code = 1, available = 0, size = 0;
    int result = -1;
    int length = connect
        ? snprintf(command, sizeof(command), "\"%s\" connect \"%s\"", adb, target)
        : snprintf(command, sizeof(command), "\"%s\" -s \"%s\" get-state", adb, target);
    if (length < 0 || (size_t)length >= sizeof(command)) return -1;
    if (!CreatePipe(&reader, &writer, &security, 0)) return -1;
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) goto done;
    null_input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, 0, NULL);
    if (null_input == INVALID_HANDLE_VALUE) goto done;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_input;
    si.hStdOutput = si.hStdError = writer;
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) goto done;
    CloseHandle(writer); writer = NULL;
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
        result = code == 0 ? 0 : -1;
    } else {
        TerminateProcess(pi.hProcess, 1);
        (void)WaitForSingleObject(pi.hProcess, 1000);
    }
    /* Both commands have tiny replies. Bound reads even if a descendant keeps
     * the pipe open; oversized replies cannot be mistaken for state=device. */
    if (PeekNamedPipe(reader, NULL, 0, NULL, &available, NULL) && available) {
        DWORD count = available < sizeof(output) - 1 ? available : (DWORD)sizeof(output) - 1;
        if (!ReadFile(reader, output, count, &size, NULL)) size = 0;
    }
    if (connect) {
        if (size) { output[size] = '\0'; LOG_INFO("adb connect: %s\n", output); }
    } else if (!otg_adb_state_is_device(output, size)) result = -1;
done:
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (writer) CloseHandle(writer);
    if (reader) CloseHandle(reader);
    return result;
}

static SOCKET connect_local(unsigned port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s); return INVALID_SOCKET;
    }
    return s;
}

static char *copy_text(const char *value) {
    size_t n = value ? strlen(value) : 0;
    char *copy = value ? malloc(n + 1) : NULL;
    if (copy) memcpy(copy, value, n + 1);
    return copy;
}

int scrcpy_session_start(struct scrcpy_session *s, const char *adb, const char *adb_serial,
                        const char *server, const char *version, bool audio, bool clipboard) {
    WSADATA wsa; char push[512], forward[128], remote[128];
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi = {0};
    SOCKET sock; unsigned port = 27183U + (GetCurrentProcessId() % 10000U);
#ifndef OTG_HAVE_AUDIO
    if (audio) return -1;
#endif
    if (!s) return -1;
    memset(s, 0, sizeof(*s)); s->audio_fd = s->control_fd = (intptr_t)INVALID_SOCKET;
    s->initialized = true;
    if (!adb || !adb_serial || !server || !version || !*adb_serial ||
        !*server || !*version || (!audio && !clipboard)) return -1;
    s->adb_path = copy_text(adb); s->adb_serial = copy_text(adb_serial);
    s->server_path = copy_text(server);
    if (!s->adb_path || !s->adb_serial || !s->server_path) goto fail;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) goto fail;
    s->winsock_ready = true;
    if (otg_adb_target_prepare(adb_serial, adb_target_command, s->adb_path)) goto fail;
    snprintf(push, sizeof(push), "push \"%s\" \"%s\"", server, REMOTE_PATH);
    if (run_adb(adb, adb_serial, push) != 0) goto fail;
    snprintf(remote, sizeof(remote), "localabstract:scrcpy_%08x", GetCurrentProcessId() & 0x7fffffffU);
    snprintf(forward, sizeof(forward), "tcp:%u %s", port, remote);
    { char forward_cmd[160]; snprintf(forward_cmd, sizeof(forward_cmd), "forward %s", forward);
      if (run_adb(adb, adb_serial, forward_cmd) != 0) goto fail; }
    s->forward_port = (intptr_t)port;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "\"%s\" -s \"%s\" shell CLASSPATH=%s app_process / com.genymobile.scrcpy.Server %s audio=%s video=false control=%s clipboard_autosync=true cleanup=true tunnel_forward=true send_device_meta=false send_dummy_byte=true send_stream_meta=true send_frame_meta=true audio_codec=raw audio_source=output scid=%08x", adb, adb_serial, REMOTE_PATH, version, audio ? "true" : "false", clipboard ? "true" : "false", GetCurrentProcessId() & 0x7fffffffU);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) goto fail;
    s->server_pid = (intptr_t)pi.hProcess; CloseHandle(pi.hThread);
    sock = INVALID_SOCKET;
    ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        sock = connect_local(port);
        if (sock != INVALID_SOCKET) {
            DWORD timeout = 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            unsigned char dummy;
            if (read_full(sock, &dummy, 1) == 1 && dummy == 0) {
                timeout = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
                break;
            }
            closesocket(sock); sock = INVALID_SOCKET;
        }
        Sleep(100);
    }
    if (sock == INVALID_SOCKET) goto fail;
    s->audio_fd = (intptr_t)sock;
    if (clipboard) {
        if (audio) s->control_fd = (intptr_t)connect_local(port);
        else { s->control_fd = s->audio_fd; s->audio_fd = (intptr_t)INVALID_SOCKET; }
        if (s->control_fd == (intptr_t)INVALID_SOCKET ||
            otg_clipboard_channel_open(&s->clipboard, s->control_fd)) goto fail;
    }
#ifdef OTG_HAVE_AUDIO
    if (audio) {
        s->player = audio_player_start(s->audio_fd);
        if (!s->player) goto fail;
    }
#endif
    s->started = true;
    LOG_INFO("scrcpy auxiliary session started (audio=%s, clipboard=%s)\n",
             audio ? "on" : "off", clipboard ? "on" : "off");
    return 0;
fail: scrcpy_session_stop(s); return -1;
}

void scrcpy_session_stop(struct scrcpy_session *s) {
    if (!s || !s->initialized) return;
#ifdef OTG_HAVE_AUDIO
    /* Pause before restoring speaker routing. Never toggle playback, reconnect
     * ADB, or affect clipboard-only/failed startup sessions. */
    if (s->started && s->player) {
        s->started = false;
        if (run_adb_timeout(s->adb_path, s->adb_serial,
                            "shell input keyevent KEYCODE_MEDIA_PAUSE", 1500))
            LOG_WARN("could not pause Android media before audio disconnect\n");
    }
#endif
    otg_clipboard_channel_close(&s->clipboard);
    if (s->control_fd != (intptr_t)INVALID_SOCKET) {
        shutdown((SOCKET)s->control_fd, SD_BOTH); closesocket((SOCKET)s->control_fd);
    }
#ifdef OTG_HAVE_AUDIO
    audio_player_stop(s->player);
#endif
    if (s->audio_fd != (intptr_t)INVALID_SOCKET) closesocket((SOCKET)s->audio_fd);
    if (s->server_pid) { TerminateProcess((HANDLE)s->server_pid, 1); WaitForSingleObject((HANDLE)s->server_pid, 1000); CloseHandle((HANDLE)s->server_pid); s->server_pid = 0; }
    if (s->forward_port && s->adb_path && s->adb_serial) {
        char command[64]; snprintf(command, sizeof(command), "forward --remove tcp:%ld", (long)s->forward_port);
        (void)run_adb(s->adb_path, s->adb_serial, command);
    }
    free(s->adb_path); free(s->adb_serial); free(s->server_path);
    s->adb_path = NULL; s->adb_serial = NULL; s->server_path = NULL;
    if (s->winsock_ready) WSACleanup();
    memset(s, 0, sizeof(*s));
    s->audio_fd = s->control_fd = (intptr_t)INVALID_SOCKET;
}
bool scrcpy_session_started(const struct scrcpy_session *s) { return s && s->started; }
#endif
