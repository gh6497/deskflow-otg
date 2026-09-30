/*
 * main.c - entry point for the deskflow-otg bridge.
 *
 * Connects to a Deskflow server as a secondary "screen" and forwards the
 * incoming keyboard/mouse events to an Android device over USB using the AOA
 * HID protocol (the same mechanism as `scrcpy --otg`).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "deskflow_client.h"

#define DEFAULT_PORT   24800
#define DEFAULT_HOST   "127.0.0.1"
#define DEFAULT_NAME   "android"
#define DEFAULT_WIDTH  1080
#define DEFAULT_HEIGHT 1920

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Bridge between a Deskflow server and an Android phone (USB AOA HID).\n"
            "\n"
            "Options:\n"
            "  -H, --host HOST       Deskflow server address (default %s)\n"
            "  -p, --port PORT       Deskflow server port (default %u)\n"
            "  -n, --name NAME       Screen name reported to the server (default %s)\n"
            "  -s, --serial SERIAL   Android USB device serial (auto-detect if omitted)\n"
            "      --width W         Virtual screen width in pixels (default %d)\n"
            "      --height H        Virtual screen height in pixels (default %d)\n"
            "  -h, --help            Show this help\n",
            prog, DEFAULT_HOST, (unsigned)DEFAULT_PORT, DEFAULT_NAME,
            DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

/* ---- deskflow callbacks -> bridge --------------------------------------- */

typedef struct {
    struct otg_bridge bridge;
    const char *serial;
} app_state;

static void cb_enter(void *ud, int16_t x, int16_t y, uint32_t seq, uint16_t mask)
{
    (void)seq;
    app_state *s = ud;
    otg_bridge_enter(&s->bridge, x, y, mask);
}

static void cb_leave(void *ud)
{
    app_state *s = ud;
    otg_bridge_leave(&s->bridge);
}

static void cb_key_down(void *ud, uint32_t keyid, uint16_t mask)
{
    (void)mask;
    app_state *s = ud;
    otg_bridge_key(&s->bridge, keyid, true);
}

static void cb_key_up(void *ud, uint32_t keyid, uint16_t mask)
{
    (void)mask;
    app_state *s = ud;
    otg_bridge_key(&s->bridge, keyid, false);
}

static void cb_key_repeat(void *ud, uint32_t keyid, uint16_t mask, int32_t count)
{
    /* Auto-repeat is implicit for a physical HID keyboard (the device repeats
     * held keys itself), so we can ignore explicit repeat notifications. */
    (void)ud;
    (void)keyid;
    (void)mask;
    (void)count;
}

static void cb_mouse_down(void *ud, uint8_t button)
{
    app_state *s = ud;
    otg_bridge_mouse_button(&s->bridge, button, true);
}

static void cb_mouse_up(void *ud, uint8_t button)
{
    app_state *s = ud;
    otg_bridge_mouse_button(&s->bridge, button, false);
}

static void cb_mouse_move(void *ud, int16_t x, int16_t y)
{
    app_state *s = ud;
    otg_bridge_mouse_move(&s->bridge, x, y);
}

static void cb_mouse_rel_move(void *ud, int16_t dx, int16_t dy)
{
    app_state *s = ud;
    otg_bridge_mouse_rel_move(&s->bridge, dx, dy);
}

static void cb_mouse_wheel(void *ud, int16_t x, int16_t y)
{
    app_state *s = ud;
    otg_bridge_mouse_wheel(&s->bridge, x, y);
}

static void cb_disconnected(void *ud)
{
    (void)ud;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;
    const char *name = DEFAULT_NAME;
    const char *serial = NULL;
    unsigned port = DEFAULT_PORT;
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if ((strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)) {
            usage(argv[0]);
            return 0;
        } else if ((strcmp(a, "-H") == 0 || strcmp(a, "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(a, "-p") == 0 || strcmp(a, "--port") == 0) && i + 1 < argc) {
            port = (unsigned)atoi(argv[++i]);
        } else if ((strcmp(a, "-n") == 0 || strcmp(a, "--name") == 0) && i + 1 < argc) {
            name = argv[++i];
        } else if ((strcmp(a, "-s") == 0 || strcmp(a, "--serial") == 0) && i + 1 < argc) {
            serial = argv[++i];
        } else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    app_state state;
    memset(&state, 0, sizeof(state));
    state.serial = serial;

    /* 1. Open the Android device and register the AOA HID keyboard + mouse. */
    if (otg_bridge_open(&state.bridge, serial) != 0) {
        return 1;
    }
    fprintf(stderr, "INFO:  AOA HID keyboard + mouse ready\n");

    /* 2. Connect to the Deskflow server. */
    df_client_callbacks cbs = {
        .on_enter = cb_enter,
        .on_leave = cb_leave,
        .on_key_down = cb_key_down,
        .on_key_up = cb_key_up,
        .on_key_repeat = cb_key_repeat,
        .on_mouse_down = cb_mouse_down,
        .on_mouse_up = cb_mouse_up,
        .on_mouse_move = cb_mouse_move,
        .on_mouse_rel_move = cb_mouse_rel_move,
        .on_mouse_wheel = cb_mouse_wheel,
        .on_disconnected = cb_disconnected,
    };

    df_client *client = df_client_connect(host, (uint16_t)port, name,
                                          width, height, &cbs, &state);
    if (!client) {
        otg_bridge_close(&state.bridge);
        return 1;
    }
    fprintf(stderr, "INFO:  connected to deskflow server %s:%u as \"%s\"\n",
            host, port, name);
    fprintf(stderr, "INFO:  move the cursor onto the \"%s\" screen to control "
                    "the phone (Ctrl+C to quit)\n", name);

    /* 3. Run the event loop. */
    int ret = df_client_run(client, &g_stop);

    df_client_disconnect(client);
    otg_bridge_close(&state.bridge);
    (void)g_stop;
    return ret;
}
