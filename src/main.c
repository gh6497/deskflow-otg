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
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include "bridge.h"
#include "build_version.h"
#include "deskflow_client.h"
#include "usb_scan.h"
#ifdef OTG_HAVE_BLE
#include "ble_hid.h"
#include "ble_supervisor.h"
#include "ble_session.h"
#include <glib.h>
#endif

#define DEFAULT_PORT   24800
#define DEFAULT_HOST   "127.0.0.1"
#define DEFAULT_NAME   "android"
#define DEFAULT_WIDTH  1080
#define DEFAULT_HEIGHT 1920

static volatile sig_atomic_t g_stop = 0;
static int g_gui = 0;

/* GUI owns stdin. A byte or EOF requests graceful shutdown, including when
 * the parent crashes. PeekNamedPipe avoids blocking on Windows console stdin. */
static int gui_stop_check(void)
{
    if (g_stop) {
        return 1;
    }
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0;
    if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL) || available) {
        g_stop = 1;
    }
#else
    struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&input, 1, 0) > 0) {
        g_stop = 1;
    }
#endif
    return g_stop != 0;
}

static void gui_event(const char *event)
{
    if (g_gui) {
        printf("{\"event\":\"%s\"}\n", event);
        fflush(stdout);
    }
}

typedef struct app_state app_state;

static void cb_connected(void *ud);

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
             "  -s, --serial SERIAL   Android USB device serial (USB transport)\n"
             "      --transport MODE usb (default) or ble (Linux)\n"
             "      --adapter NAME    BlueZ adapter for BLE (default hci0)\n"
             "      --ble-isolate     Linux: sudo native BLE-only supervisor; restores on exit\n"
             "      --ble-restore     Linux: recover saved adapter settings without Python\n"
             "      --ble-service     Linux: resident user GATT service (keep running)\n"
             "      --ble-session     Linux: BLE bridge via an existing resident service\n"
            "      --width W         Virtual screen width, 1..32767 (default %d)\n"
            "      --height H        Virtual screen height, 1..32767 (default %d)\n"
            "      --mouse-mode MODE absolute (default) or relative (compatibility)\n"
            "      --gui             JSON status on stdout; stdin byte/EOF stops\n"
            "      --usb-scan        List USB attachments as JSON (no ADB or HID)\n"
            "      --usb-watch       Watch USB hotplug as JSON lines; stdin/EOF stops\n"
            "  -h, --help            Show this help\n"
            "  -v, --version         Show software version and Git commit\n"
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

struct app_state {
    struct otg_bridge bridge;
    const char *serial;
    void *transport_owner;
    bool resident_session;
    bool deskflow_connected;
    bool input_ready_reported;
};

static void cb_connected(void *ud)
{
    app_state *s = ud;
    s->deskflow_connected = true;
    if (otg_bridge_ready(&s->bridge)) {
        s->input_ready_reported = true;
        gui_event("connected");
    }
}

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
    app_state *s = ud;
    if (!s->transport_owner) g_stop = 1;
}

static int cb_idle(void *ud)
{
    /* Detect a USB unplug even while no HID event is in flight.  Returning
     * non-zero stops the loop, which closes the TCP connection so the deskflow
     * server notices the client is gone and returns the cursor to the primary
     * screen. */
    app_state *s = ud;
    if (!otg_bridge_check(&s->bridge)) {
        return 1;
    }
    if (s->transport_owner && s->input_ready_reported &&
        !otg_bridge_ready(&s->bridge)) {
        return 1; /* Drop TCP so Deskflow returns the cursor to the host. */
    }
    if (s->deskflow_connected && !s->input_ready_reported &&
        otg_bridge_ready(&s->bridge)) {
        s->input_ready_reported = true;
        gui_event("connected");
    }
    return 0;
}

static void destroy_transport_owner(app_state *s)
{
#ifdef OTG_HAVE_BLE
    if (s->transport_owner) {
        if(s->resident_session)otg_ble_session_destroy(s->transport_owner);
        else otg_ble_hid_destroy(s->transport_owner);
        s->transport_owner = NULL;
    }
#else
    (void)s;
#endif
}

#ifdef OTG_HAVE_BLE
static app_state *ble_app;
static int ble_stop_check(void)
{
    if (g_stop || (g_gui && gui_stop_check())) return 1;
    if (!ble_app || !otg_bridge_check(&ble_app->bridge)) return 1;
    return ble_app->input_ready_reported && !otg_bridge_ready(&ble_app->bridge);
}

static bool ble_wait(app_state *s, bool require_ready, unsigned delay_ms)
{
    gint64 until=g_get_monotonic_time()+(gint64)delay_ms*1000;
    while (!g_stop && !(g_gui && gui_stop_check())) {
        if (!otg_bridge_check(&s->bridge)) return false;
        if (require_ready && otg_bridge_ready(&s->bridge)) return true;
        if (!require_ready && g_get_monotonic_time()>=until) return true;
        g_usleep(5000);
    }
    return false;
}
#endif

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;
    const char *name = DEFAULT_NAME;
    const char *serial = NULL;
    const char *transport = "usb";
    const char *adapter = "hci0";
    int ble_isolate = 0, ble_restore = 0;
    int ble_service = 0, ble_session = 0;
    unsigned port = DEFAULT_PORT;
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    enum otg_mouse_mode mouse_mode = OTG_MOUSE_ABSOLUTE;

    if (argc == 2 && strcmp(argv[1], "--usb-scan") == 0) {
        return otg_usb_scan();
    }
    if (argc == 2 && strcmp(argv[1], "--usb-watch") == 0) {
        g_gui = 1;
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        return otg_usb_watch(gui_stop_check);
    }

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("deskflow-otg %s (commit %s)\n", OTG_VERSION, OTG_COMMIT);
            return 0;
        } else if ((strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)) {
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
        } else if (strcmp(a, "--transport") == 0 && i + 1 < argc) {
            transport = argv[++i];
        } else if (strcmp(a, "--adapter") == 0 && i + 1 < argc) {
            adapter = argv[++i];
        } else if (strcmp(a, "--ble-isolate") == 0) {
            ble_isolate = 1;
        } else if (strcmp(a, "--ble-restore") == 0) {
            ble_restore = 1;
        } else if (strcmp(a, "--ble-service") == 0) {
            ble_service = 1;
        } else if (strcmp(a, "--ble-session") == 0) {
            ble_session = 1;
        } else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
            width = parse_screen_size(argv[++i]);
        } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
            height = parse_screen_size(argv[++i]);
        } else if (strcmp(a, "--gui") == 0) {
            g_gui = 1;
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
    if(ble_service || ble_session) {
        if(g_gui || (ble_service && (ble_session || ble_isolate || ble_restore || serial)) ||
           (ble_session && (ble_restore || strcmp(transport,"ble") || mouse_mode!=OTG_MOUSE_ABSOLUTE))) {
            fprintf(stderr,"ERROR: --ble-service is standalone; use --ble-session with --transport ble and absolute mouse mode in CLI\n");return 2;
        }
#ifdef OTG_HAVE_BLE
        if(ble_service)return otg_ble_service_run(adapter,&g_stop);
#else
        fprintf(stderr,"ERROR: resident BLE requires BUILD_BLE=ON\n");return 2;
#endif
    }
    if (ble_isolate || ble_restore) {
        if (g_gui || (ble_isolate && ble_restore) ||
            (ble_isolate && strcmp(transport, "ble"))) {
            fprintf(stderr, "ERROR: use --ble-isolate with --transport ble in CLI; --ble-restore is separate\n");
            return 2;
        }
#ifdef OTG_HAVE_BLE
        return otg_ble_supervise(argc, argv, adapter, ble_restore, &g_stop);
#else
        fprintf(stderr, "ERROR: BLE supervision requires Linux with BUILD_BLE=ON (adapter %s)\n", adapter);
        return 2;
#endif
    }
    if (g_gui) {
#ifndef _WIN32
        /* A vanished GUI closes stdout as well as stdin. Let the cancellation
         * path release HID state rather than exiting on a broken status pipe. */
        signal(SIGPIPE, SIG_IGN);
#endif
        df_client_set_stop_check(gui_stop_check);
        if (gui_stop_check()) {
            return 0;
        }
    }

    app_state state;
    memset(&state, 0, sizeof(state));
    state.serial = serial;

    /* 1. Open the selected HID transport and register keyboard + pointer. */
    if (strcmp(transport, "usb") == 0) {
        if (otg_bridge_open(&state.bridge, serial, width, height, mouse_mode) != 0) {
            return 1;
        }
        fprintf(stderr, "INFO:  AOA HID keyboard + %s pointer ready (screen %dx%d)\n",
                mouse_mode == OTG_MOUSE_ABSOLUTE ? "absolute" : "relative", width, height);
    } else if (strcmp(transport, "ble") == 0) {
#ifdef OTG_HAVE_BLE
        state.resident_session=ble_session;
        state.transport_owner=ble_session ? (void *)otg_ble_session_open(adapter) :
                                           (void *)otg_ble_hid_create(adapter);
        const struct otg_hid_transport_ops *ops=ble_session ? otg_ble_session_ops() : otg_ble_hid_ops();
        if (!state.transport_owner || otg_bridge_open_transport(&state.bridge, ops, state.transport_owner,
                                              width, height, mouse_mode) != 0) {
            destroy_transport_owner(&state);
            return 1;
        }
        fprintf(stderr, "INFO:  BLE HID advertisement started on %s; waiting for phone subscriptions\n", adapter);
#else
        fprintf(stderr, "ERROR: BLE HID requires Linux with BUILD_BLE=ON (adapter %s)\n", adapter);
        return 2;
#endif
    } else {
        fprintf(stderr, "invalid transport: %s (use usb or ble)\n", transport);
        return 2;
    }
    if (g_gui && strcmp(transport, "usb") == 0) {
        printf("{\"event\":\"usb-ready\",\"usb-id\":\"%u:%u\"}\n",
               (unsigned)libusb_get_bus_number(state.bridge.aoa.device),
               (unsigned)libusb_get_device_address(state.bridge.aoa.device));
        fflush(stdout);
    }
    if (g_gui && gui_stop_check()) {
        otg_bridge_close(&state.bridge);
        destroy_transport_owner(&state);
        return 0;
    }

    /* 2. Connect to the Deskflow server. */
    df_client_callbacks cbs = {
        .on_connected = cb_connected,
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
        .on_tick = state.transport_owner ? cb_idle : NULL,
        .tick_interval_ms = state.transport_owner ? 5 : 0,
    };

    int ret = 0;
#ifdef OTG_HAVE_BLE
    if (state.transport_owner) {ble_app=&state;df_client_set_stop_check(ble_stop_check);}
#endif
    do {
#ifdef OTG_HAVE_BLE
        if (state.transport_owner && !ble_wait(&state,true,0)) break;
#endif
        df_client *client=df_client_connect(host,(uint16_t)port,name,width,height,&cbs,&state);
        if(client) {
            fprintf(stderr,"INFO: connected to deskflow server %s:%u as \"%s\"\n",host,port,name);
            ret=df_client_run(client,&g_stop);
            df_client_disconnect(client);
        } else ret=1;
        if(!state.transport_owner || g_stop) break;
        otg_bridge_leave(&state.bridge);
        otg_bridge_reset_input(&state.bridge);
        state.deskflow_connected=state.input_ready_reported=false;
        gui_event("disconnected");
        fprintf(stderr,"INFO: BLE service retained; reconnect phone without removing pairing; retrying Deskflow when ready\n");
#ifdef OTG_HAVE_BLE
        if(!ble_wait(&state,false,1000)) break;
#endif
    } while(!g_stop);
    otg_bridge_close(&state.bridge);
    destroy_transport_owner(&state);
    gui_event("disconnected");
    (void)g_stop;
    return ret;
}
