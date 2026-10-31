/* Windows audio sink for the scrcpy raw audio stream.
 * The protocol and server lifecycle match scrcpy; WASAPI replaces SDL/ALSA.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_session.h"

#ifdef OTG_HAVE_AUDIO
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID OTG_CLSID_MMDeviceEnumerator =
    {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID OTG_IID_IMMDeviceEnumerator =
    {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID OTG_IID_IAudioClient =
    {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const GUID OTG_IID_IAudioRenderClient =
    {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

#define REMOTE_PATH "/data/local/tmp/deskflow-otg-scrcpy-server.jar"
#define META_SIZE 64U
#define HEADER_SIZE 12U
#define MAX_FRAME (1024U * 2U * 2U)
#define CODEC_RAW UINT32_C(0x00726177)

#define LOG_ERR(...) fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int read_full(SOCKET s, void *out, size_t n) {
    unsigned char *p = out;
    while (n) {
        int got = recv(s, (char *)p, (int)n, 0);
        if (got <= 0) return got == 0 ? 0 : -1;
        p += got; n -= (size_t)got;
    }
    return 1;
}

static int run_adb(const char *adb, const char *serial, const char *command_line) {
    char command[2048];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    snprintf(command, sizeof(command), "\"%s\" -s \"%s\" %s",
             adb, serial, command_line);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0 ? 0 : -1;
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

static int open_wasapi(IAudioClient **client, IAudioRenderClient **render) {
    IMMDeviceEnumerator *enumerator = NULL; IMMDevice *device = NULL;
    WAVEFORMATEX format = { WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
    REFERENCE_TIME duration = 1000000;
    HRESULT hr = CoCreateInstance(&OTG_CLSID_MMDeviceEnumerator, NULL,
                                  CLSCTX_ALL, &OTG_IID_IMMDeviceEnumerator,
                                  (void **)&enumerator);
    if (FAILED(hr)) return -1;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->lpVtbl->Activate(device, &OTG_IID_IAudioClient,
                                                      CLSCTX_ALL, NULL,
                                                      (void **)client);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->Initialize(*client,
        AUDCLNT_SHAREMODE_SHARED, 0, duration, 0, &format, NULL);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->GetService(*client,
        &OTG_IID_IAudioRenderClient, (void **)render);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->Start(*client);
    if (device) device->lpVtbl->Release(device);
    if (enumerator) enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        if (*render) (*render)->lpVtbl->Release(*render);
        if (*client) (*client)->lpVtbl->Release(*client);
        *render = NULL; *client = NULL; return -1;
    }
    return 0;
}

static int play_pcm(IAudioClient *client, IAudioRenderClient *render,
                    const unsigned char *data, size_t size) {
    size_t frames = size / 4;
    while (frames) {
        UINT32 available = 0;
        BYTE *buffer = NULL;
        if (FAILED(client->lpVtbl->GetCurrentPadding(client, &available))) return -1;
        UINT32 capacity = 48000U - available;
        if (!capacity) { Sleep(2); continue; }
        UINT32 count = (UINT32)(frames < capacity ? frames : capacity);
        if (FAILED(render->lpVtbl->GetBuffer(render, count, &buffer))) return -1;
        memcpy(buffer, data, (size_t)count * 4);
        if (FAILED(render->lpVtbl->ReleaseBuffer(render, count, 0))) return -1;
        data += (size_t)count * 4; frames -= count;
    }
    return 0;
}

static DWORD WINAPI audio_thread(void *arg) {
    struct audio_session *s = arg; unsigned char meta[META_SIZE], codec[4], h[HEADER_SIZE];
    unsigned char *payload = NULL; IAudioClient *client = NULL; IAudioRenderClient *render = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (read_full((SOCKET)s->socket_fd, meta, sizeof(meta)) != 1 ||
        read_full((SOCKET)s->socket_fd, codec, 4) != 1 || be32(codec) != CODEC_RAW) goto done;
    if (open_wasapi(&client, &render) != 0) { LOG_ERR("could not open WASAPI playback device\n"); goto done; }
    while (!s->stop_requested && read_full((SOCKET)s->socket_fd, h, sizeof(h)) == 1) {
        uint32_t n = be32(h + 8);
        if (!n || n > MAX_FRAME) break;
        unsigned char *p = realloc(payload, n); if (!p) break; payload = p;
        if (read_full((SOCKET)s->socket_fd, payload, n) != 1 || play_pcm(client, render, payload, n) != 0) break;
    }
done:
    free(payload);
    if (client) { client->lpVtbl->Stop(client); client->lpVtbl->Release(client); }
    if (render) render->lpVtbl->Release(render);
    CoUninitialize();
    return 0;
}

int audio_session_start(struct audio_session *s, const char *adb, const char *serial,
                        const char *server, const char *version) {
    WSADATA wsa; char push[512], forward[128], remote[128];
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi = {0};
    SOCKET sock; unsigned port = 27183U + (GetCurrentProcessId() % 10000U);
    if (!s || !adb || !serial || !server || !version) return -1;
    memset(s, 0, sizeof(*s)); s->socket_fd = (intptr_t)INVALID_SOCKET;
    s->adb_path = copy_text(adb); s->serial = copy_text(serial);
    s->server_path = copy_text(server);
    if (!s->adb_path || !s->serial || !s->server_path) goto fail;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    snprintf(push, sizeof(push), "push \"%s\" \"%s\"", server, REMOTE_PATH);
    if (run_adb(adb, serial, push) != 0) goto fail;
    snprintf(remote, sizeof(remote), "localabstract:scrcpy_%08x", GetCurrentProcessId() & 0x7fffffffU);
    snprintf(forward, sizeof(forward), "tcp:%u tcp:%s", port, remote);
    { char forward_cmd[160]; snprintf(forward_cmd, sizeof(forward_cmd), "forward %s", forward);
      if (run_adb(adb, serial, forward_cmd) != 0) goto fail; }
    s->forward_port = (intptr_t)port;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "\"%s\" -s \"%s\" shell CLASSPATH=%s app_process / com.genymobile.scrcpy.Server %s audio=true video=false control=false cleanup=true tunnel_forward=true send_device_meta=true send_dummy_byte=false send_stream_meta=true send_frame_meta=true audio_codec=raw audio_source=output scid=%08x", adb, serial, REMOTE_PATH, version, GetCurrentProcessId() & 0x7fffffffU);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) goto fail;
    s->server_pid = (intptr_t)pi.hProcess; CloseHandle(pi.hThread);
    sock = INVALID_SOCKET;
    for (unsigned attempt = 0; attempt < 100 && sock == INVALID_SOCKET; ++attempt) {
        sock = connect_local(port);
        if (sock == INVALID_SOCKET) Sleep(100);
    }
    if (sock == INVALID_SOCKET) goto fail;
    s->socket_fd = (intptr_t)sock; s->stop_requested = false;
    HANDLE thread = CreateThread(NULL, 0, audio_thread, s, 0, NULL);
    if (!thread) goto fail;
    s->thread = thread; s->started = true; LOG_INFO("scrcpy audio forwarding started (WASAPI, raw PCM)\n");
    return 0;
fail: audio_session_stop(s); WSACleanup(); return -1;
}

void audio_session_stop(struct audio_session *s) {
    if (!s) return; s->stop_requested = true;
    if (s->socket_fd != (intptr_t)INVALID_SOCKET) { shutdown((SOCKET)s->socket_fd, SD_BOTH); closesocket((SOCKET)s->socket_fd); s->socket_fd = (intptr_t)INVALID_SOCKET; }
    if (s->thread) { WaitForSingleObject((HANDLE)s->thread, 5000); CloseHandle((HANDLE)s->thread); s->thread = NULL; }
    if (s->server_pid) { TerminateProcess((HANDLE)s->server_pid, 1); WaitForSingleObject((HANDLE)s->server_pid, 1000); CloseHandle((HANDLE)s->server_pid); s->server_pid = 0; }
    if (s->forward_port && s->adb_path && s->serial) {
        char command[64]; snprintf(command, sizeof(command), "forward --remove tcp:%ld", (long)s->forward_port);
        (void)run_adb(s->adb_path, s->serial, command);
    }
    free(s->adb_path); free(s->serial); free(s->server_path);
    s->adb_path = NULL; s->serial = NULL; s->server_path = NULL;
    s->started = false; WSACleanup();
}
bool audio_session_started(const struct audio_session *s) { return s && s->started; }
#endif
