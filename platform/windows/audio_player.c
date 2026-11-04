/* Windows playback for the scrcpy raw audio stream.
 * Protocol reference: scrcpy demuxer/audio raw stream; WASAPI replaces SDL.
 * SPDX-License-Identifier: Apache-2.0 */
#include "audio_player.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>
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

#define HEADER_SIZE 12U
#define MAX_FRAME (1024U * 2U * 2U)
#define CODEC_RAW UINT32_C(0x00726177)
#define LOG_ERR(...) fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_HR(operation, hr) \
    fprintf(stderr, "ERROR: %s failed (HRESULT=0x%08lx)\n", \
            operation, (unsigned long)(hr))

struct audio_player {
    SOCKET socket_fd;
    HANDLE thread;
    volatile LONG ready;
};

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

static int open_wasapi(IAudioClient **client, IAudioRenderClient **render,
                       UINT32 *buffer_size) {
    IMMDeviceEnumerator *enumerator = NULL; IMMDevice *device = NULL;
    WAVEFORMATEX format = { WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
    REFERENCE_TIME duration = 1000000;
    HRESULT hr = CoCreateInstance(&OTG_CLSID_MMDeviceEnumerator, NULL,
                                  CLSCTX_ALL, &OTG_IID_IMMDeviceEnumerator,
                                  (void **)&enumerator);
    if (FAILED(hr)) { LOG_HR("CoCreateInstance", hr); return -1; }
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->lpVtbl->Activate(device, &OTG_IID_IAudioClient,
                                                      CLSCTX_ALL, NULL,
                                                      (void **)client);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->Initialize(*client,
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        duration, 0, &format, NULL);
    if (FAILED(hr)) LOG_HR("IAudioClient::Initialize", hr);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->GetBufferSize(*client, buffer_size);
    if (FAILED(hr)) LOG_HR("IAudioClient::GetBufferSize", hr);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->GetService(*client,
        &OTG_IID_IAudioRenderClient, (void **)render);
    if (FAILED(hr)) LOG_HR("IAudioClient::GetService", hr);
    if (SUCCEEDED(hr)) hr = (*client)->lpVtbl->Start(*client);
    if (FAILED(hr)) LOG_HR("IAudioClient::Start", hr);
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
                    UINT32 buffer_size, const unsigned char *data, size_t size) {
    size_t frames = size / 4;
    while (frames) {
        UINT32 available = 0;
        BYTE *buffer = NULL;
        if (FAILED(client->lpVtbl->GetCurrentPadding(client, &available))) return -1;
        UINT32 capacity = buffer_size > available ? buffer_size - available : 0;
        if (!capacity) { Sleep(2); continue; }
        UINT32 count = (UINT32)(frames < capacity ? frames : capacity);
        HRESULT hr = render->lpVtbl->GetBuffer(render, count, &buffer);
        if (FAILED(hr)) { LOG_HR("IAudioRenderClient::GetBuffer", hr); return -1; }
        memcpy(buffer, data, (size_t)count * 4);
        hr = render->lpVtbl->ReleaseBuffer(render, count, 0);
        if (FAILED(hr)) { LOG_HR("IAudioRenderClient::ReleaseBuffer", hr); return -1; }
        data += (size_t)count * 4; frames -= count;
    }
    return 0;
}

static DWORD WINAPI audio_thread(void *arg) {
    struct audio_player *player = arg; unsigned char codec[4], h[HEADER_SIZE];
    unsigned char *payload = NULL; IAudioClient *client = NULL; IAudioRenderClient *render = NULL;
    UINT32 buffer_size = 0; HRESULT com_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com_initialized = SUCCEEDED(com_hr);
    if (FAILED(com_hr)) { LOG_HR("CoInitializeEx", com_hr); goto done; }
    if (read_full(player->socket_fd, codec, 4) != 1) goto done;
    if (be32(codec) == 0) { LOG_ERR("Android did not provide an audio stream\n"); goto done; }
    if (be32(codec) != CODEC_RAW) {
        LOG_ERR("scrcpy audio codec is not raw PCM; use audio_codec=raw\n"); goto done;
    }
    if (open_wasapi(&client, &render, &buffer_size) != 0) {
        LOG_ERR("could not open WASAPI playback device\n"); goto done;
    }
    InterlockedExchange(&player->ready, 1);
    while (read_full(player->socket_fd, h, sizeof(h)) == 1) {
        uint32_t n = be32(h + 8);
        if (!n || n > MAX_FRAME) break;
        unsigned char *p = realloc(payload, n); if (!p) break; payload = p;
        if (read_full(player->socket_fd, payload, n) != 1 ||
            play_pcm(client, render, buffer_size, payload, n) != 0) break;
    }
done:
    InterlockedExchange(&player->ready, 0);
    free(payload);
    if (client) { client->lpVtbl->Stop(client); client->lpVtbl->Release(client); }
    if (render) render->lpVtbl->Release(render);
    if (com_initialized) CoUninitialize();
    return 0;
}

struct audio_player *audio_player_start(intptr_t socket_fd) {
    struct audio_player *player = malloc(sizeof(*player));
    if (!player) return NULL;
    player->socket_fd = (SOCKET)socket_fd;
    player->ready = 0;
    player->thread = CreateThread(NULL, 0, audio_thread, player, 0, NULL);
    if (!player->thread) { free(player); return NULL; }
    return player;
}

bool audio_player_ready(struct audio_player *player) {
    return player && InterlockedCompareExchange(&player->ready, 0, 0) != 0;
}

void audio_player_stop(struct audio_player *player) {
    if (!player) return;
    shutdown(player->socket_fd, SD_BOTH);
    WaitForSingleObject(player->thread, INFINITE);
    CloseHandle(player->thread);
    free(player);
}
