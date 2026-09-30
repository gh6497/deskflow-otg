/*
 * main.c - entry point for the deskflow-otg bridge.
 *
 * Connects to a Deskflow server as a secondary "screen" and forwards the
 * incoming keyboard/mouse events to an Android device over USB using the AOA
 * HID protocol (the same mechanism as `scrcpy --otg`).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
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
            "      --width W         Virtual screen width, 1..32767 (default %d)\n"
            "      --height H        Virtual screen height, 1..32767 (default %d)\n"
            "      --mouse-mode MODE absolute (default) or relative (compatibility)\n"
            "  -h, --help            Show this help\n"
            "\n"
            "Absolute mode maps the virtual screen to the full phone display.\n"
            "Use the phone's resolution/aspect ratio for natural movement.\n"
            "Relative mode may drift due to Android pointer speed/acceleration.\n",
            prog, DEFAULT_HOST, (unsigned)DEFAULT_PORT, DEFAULT_NAME,
            DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

static int parse_screen_size(const char *arg)
{
    char *end;
    errno = 0;
    long size = strtol(arg, &end, 10);
    if (errno || end == arg || *end || size < 1 || size > INT16_MAX) {
        return 0;
    }
    return (int)size;
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

static int cb_idle(void *ud)
{
    /* Detect a USB unplug even while no HID event is in flight.  Returning
     * non-zero stops the loop, which closes the TCP connection so the deskflow
     * server notices the client is gone and returns the cursor to the primary
     * screen. */
    app_state *s = ud;
    return otg_bridge_check(&s->bridge) ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;
    const char *name = DEFAULT_NAME;
    const char *serial = NULL;
    unsigned port = DEFAULT_PORT;
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    enum otg_mouse_mode mouse_mode = OTG_MOUSE_ABSOLUTE;

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
            width = parse_screen_size(argv[++i]);
        } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
            height = parse_screen_size(argv[++i]);
        } else if (strcmp(a, "--mouse-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "absolute") == 0) {
                mouse_mode = OTG_MOUSE_ABSOLUTE;
            } else if (strcmp(mode, "relative") == 0) {
                mouse_mode = OTG_MOUSE_RELATIVE;
            } else {
                fprintf(stderr, "invalid mouse mode: %s (use absolute or relative)\n", mode);
                return 2;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!width || !height) {
        fprintf(stderr, "screen width and height must be integers in 1..32767\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    app_state state;
    memset(&state, 0, sizeof(state));
    state.serial = serial;

    /* 1. Open the Android device and register the AOA HID keyboard + mouse. */
    if (otg_bridge_open(&state.bridge, serial, width, height, mouse_mode) != 0) {
        return 1;
    }
    fprintf(stderr, "INFO:  AOA HID keyboard + %s pointer ready (screen %dx%d)\n",
            mouse_mode == OTG_MOUSE_ABSOLUTE ? "absolute" : "relative", width, height);

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
        .on_idle = cb_idle,
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
                    "the phone; move it back across the edge to return\n", name);
    fprintf(stderr, "INFO:  to quit, first move the cursor back to this screen, "
                    "then press Ctrl+C (or just unplug the phone)\n");

    /* 3. Run the event loop. */
    int ret = df_client_run(client, &g_stop);

    df_client_disconnect(client);
    otg_bridge_close(&state.bridge);
    (void)g_stop;
    return ret;
}
