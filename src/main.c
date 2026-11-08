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
#include "scrcpy_session.h"
#ifdef OTG_HAVE_AUDIO
#include "audio_player.h"
#endif
#include "adb_display.h"
#ifdef OTG_HAVE_BLE
#include "ble_hid.h"
#include "ble_supervisor.h"
#include "ble_session.h"
#include <glib.h>
#endif
#ifdef OTG_HAVE_BLE_WIN
#include "ble_win.h"
#include "ble_win_control.h"
static int g_ble_control = 0;
static struct otg_ble_control g_control;
#endif

#define DEFAULT_PORT   24800
#define DEFAULT_HOST   "127.0.0.1"
#define DEFAULT_NAME   "android"
#define DEFAULT_WIDTH  1080
#define DEFAULT_HEIGHT 1920

/* Parsed connection configuration; runtime resources belong to app_state. */
struct app_options {
    /* Deskflow connection and virtual screen. */
    const char *host;
    unsigned port;
    const char *name;
    int width;
    int height;
    df_client_tls_options tls;

    /* HID transport and input behavior. */
    const char *serial;
    const char *transport;
    const char *adapter;
    const char *ble_peer;
    int ble_isolate;
    int ble_restore;
    int ble_service;
    int ble_session;
    int ble_status;
    int ble_stop_service;
#ifdef OTG_HAVE_BLE_WIN
    bool adapter_explicit;
#endif
#ifdef _WIN32
    int background;
    int background_child;
    int background_stop;
#endif
    enum otg_mouse_mode mouse_mode;
    enum otg_middle_button_action middle_button_action;
    enum otg_right_button_action right_button_action;

    /* Optional ADB features and unlock configuration. */
    const char *adb_path;
    const char *adb_serial;
    const char *scrcpy_server;
    const char *scrcpy_version;
    int audio;
    int clipboard;
    int auto_rotate;
    const char *unlock_pin;
    const char *unlock_pin_file;
    int unlock_adb_gate;
};

static volatile sig_atomic_t g_stop = 0;
static int g_gui = 0;
#ifdef _WIN32
static HANDLE g_ble_stop_event = NULL;
static HANDLE g_ble_instance_mutex = NULL;
#endif

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
    if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
        g_stop = 1;
    }
#ifdef OTG_HAVE_BLE_WIN
    else if (g_ble_control && available) {
        char bytes[1536];
        DWORD read = 0;
        if (!ReadFile(input, bytes, available < sizeof(bytes) ? available : (DWORD)sizeof(bytes), &read, NULL) ||
            !read || !otg_ble_control_feed(&g_control, bytes, read)) {
            fprintf(stderr, "ERROR: invalid Windows BLE GUI control stream\n");
            g_stop = 1;
        }
        if (g_control.stop) g_stop = 1;
    }
#endif
    else if (available) g_stop = 1;
#else
    struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&input, 1, 0) > 0) {
        g_stop = 1;
    }
#endif
#ifdef OTG_HAVE_BLE_WIN
    if (g_ble_control && g_control.phase != OTG_BLE_ACTIVE) return 1;
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

#ifdef _WIN32
/* The CRT SIGINT handler is not delivered consistently for every Windows
 * console host (notably when the process is waiting in WinSock/WinRT code).
 * Use the native console control path as well; the handler only sets the
 * async-safe stop flag and all cleanup remains on the main thread. */
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = 1;
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

#ifdef _WIN32
static int windows_ble_stop_background(void)
{
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\DeskflowOtgBleStop");
    if (!event) {
        fprintf(stderr, "INFO: no Windows BLE background process is running\n");
        return 1;
    }
    SetEvent(event);
    CloseHandle(event);
    fprintf(stderr, "INFO: Windows BLE background stop requested\n");
    return 0;
}

static int windows_ble_start_background_child(void)
{
    const wchar_t *original = GetCommandLineW();
    size_t length = wcslen(original);
    size_t extra = wcslen(L" --background-child") + 1;
    wchar_t *command = HeapAlloc(GetProcessHeap(), 0,
                                 (length + extra) * sizeof(*command));
    if (!command) {
        fprintf(stderr, "ERROR: could not allocate background command line\n");
        return 1;
    }
    memcpy(command, original, length * sizeof(*command));
    wcscpy(command + length, L" --background-child");

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    BOOL created = CreateProcessW(NULL, command, NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                                  NULL, NULL, &startup, &process);
    HeapFree(GetProcessHeap(), 0, command);
    if (!created) {
        fprintf(stderr, "ERROR: could not start Windows BLE background process (%lu)\n",
                (unsigned long)GetLastError());
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    fprintf(stderr, "INFO: Windows BLE is now running in the background\n");
    return 0;
}

static int windows_ble_background_init(void)
{
    g_ble_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\DeskflowOtgBleInstance");
    if (!g_ble_instance_mutex) {
        fprintf(stderr, "ERROR: could not create Windows BLE instance mutex\n");
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "ERROR: Windows BLE background process is already running\n");
        CloseHandle(g_ble_instance_mutex);
        g_ble_instance_mutex = NULL;
        return 0;
    }
    g_ble_stop_event = CreateEventW(NULL, TRUE, FALSE, L"Local\\DeskflowOtgBleStop");
    if (!g_ble_stop_event) {
        fprintf(stderr, "ERROR: could not create Windows BLE stop event\n");
        CloseHandle(g_ble_instance_mutex);
        g_ble_instance_mutex = NULL;
        return 0;
    }
    return 1;
}

static void windows_ble_background_cleanup(void)
{
    if (g_ble_stop_event) CloseHandle(g_ble_stop_event);
    if (g_ble_instance_mutex) CloseHandle(g_ble_instance_mutex);
    g_ble_stop_event = NULL;
    g_ble_instance_mutex = NULL;
}
#endif

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Bridge between a Deskflow server and an Android phone (USB AOA / optional BLE HID).\n"
            "\n"
            "Options:\n"
            "  -H, --host HOST       Deskflow server address (default %s)\n"
            "  -p, --port PORT       Deskflow server port (default %u)\n"
            "  -n, --name NAME       Screen name reported to the server (default %s)\n"
             "  -s, --serial SERIAL   Android USB device serial (USB transport)\n"
              "      --transport MODE usb (default) or ble (Linux/experimental Windows)\n"
             "      --adapter NAME    BlueZ adapter for BLE (default hci0)\n"
             "      --ble-isolate     Linux: sudo native BLE-only supervisor; restores on exit\n"
             "      --ble-restore     Linux: recover saved adapter settings without Python\n"
             "      --ble-service     Linux: resident user GATT service (keep running)\n"
              "      --ble-session     Linux: BLE bridge via an existing resident service\n"
              "      --ble-peer MAC    Target phone identity (Linux) / Windows BLE address (required)\n"
              "      --background       Windows BLE: keep the provider alive after closing the console\n"
              "      --background-stop  Stop the Windows BLE background process\n"
              "      --ble-status      Read resident service JSON status without attaching\n"
             "      --ble-stop-service Stop an idle user service (loses GATT subscription continuity)\n"
             "      --width W         Virtual screen width, 1..32767 (default %d)\n"
             "      --height H        Virtual screen height, 1..32767 (default %d)\n"
             "      --mouse-mode MODE absolute (default) or relative (compatibility)\n"
              "      --middle-button-action MODE home (default) or normal (Android Meta+Enter)\n"
              "      --right-button-action MODE back (default) or normal (Android Meta+Backspace)\n"
              "      OTG_UNLOCK_PIN       GUI/experimental numeric PIN sent once on first enter\n"
             "      --audio             Forward Android output audio through ADB (Linux/Windows)\n"
              "      --clipboard         Share Deskflow text clipboard through ADB/scrcpy\n"
              "      --tls               Use Deskflow TLS for the server connection\n"
              "      --tls-cert FILE     PEM client certificate and private key (optional)\n"
              "      --tls-fingerprint HEX  Trusted server SHA-256 fingerprint\n"
              "      --tls-fingerprint-file FILE  Deskflow trusted fingerprint database\n"
              "      --auto-rotate       Follow Android rotation and update screen size (requires ADB feature)\n"
              "      --adb PATH          ADB executable for audio/clipboard (default adb)\n"
              "      --adb-serial SERIAL ADB transport (default USB serial; may be host:port)\n"
              "      --scrcpy-server PATH  Matching scrcpy-server jar/apk for ADB features\n"
             "      --scrcpy-version VER  Matching scrcpy server version\n"
             "      --gui             JSON status on stdout; stdin byte/EOF stops\n"
            "      --gui-ble-control Windows GUI: pause/resume sessions without recreating GATT\n"
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
    struct scrcpy_session scrcpy;
    struct otg_display_monitor *display;
    struct otg_display_size display_size;
    bool display_pending;
    bool unlock_adb_gate;
    bool unlock_lock_query_pending;
    const char *serial;
    void *transport_owner;
    bool resident_session;
    bool deskflow_connected;
    bool input_ready_reported;
    bool audio_ready_reported;
    df_client *client;
    bool clipboard_error;
    unsigned held_key_count;
    struct { uint32_t id; bool down; } held_keys[64];
};

static void clipboard_failed(app_state *s)
{
    if (s->clipboard_error) return;
    fprintf(stderr, "WARN: clipboard unavailable; keyboard/mouse connection retained (reconnect to retry)\n");
    gui_event("clipboard-unavailable");
    s->clipboard_error = true;
    otg_clipboard_channel_close(&s->scrcpy.clipboard);
    otg_bridge_release_keyboard(&s->bridge);
    s->held_key_count = 0;
    if (s->client) df_client_enable_clipboard(s->client, false);
}

static void cb_clipboard_enabled(void *ud, bool enabled)
{
    app_state *s = ud;
    if (!enabled) {
        free(s->scrcpy.clipboard.queued); s->scrcpy.clipboard.queued = NULL;
        s->scrcpy.clipboard.queued_size = 0;
        if (s->held_key_count) otg_bridge_release_keyboard(&s->bridge);
        s->held_key_count = 0;
    }
    if (!s->clipboard_error) gui_event(enabled ? "clipboard-ready" : "clipboard-disabled");
}

static void cb_clipboard(void *ud, const char *text, size_t size)
{
    app_state *s = ud;
    if (otg_clipboard_channel_set(&s->scrcpy.clipboard, text, size)) clipboard_failed(s);
}

static void cb_phone_clipboard(void *ud, const char *text, size_t size)
{
    app_state *s = ud;
    if (s->client && df_client_set_clipboard(s->client, text, size)) clipboard_failed(s);
}

static void clipboard_poll(app_state *s)
{
    if (!s->scrcpy.clipboard.ready || s->clipboard_error) return;
    if (otg_clipboard_channel_poll(&s->scrcpy.clipboard, cb_phone_clipboard, s)) {
        clipboard_failed(s);
        return;
    }
    if (!otg_clipboard_channel_busy(&s->scrcpy.clipboard)) {
        for (unsigned i = 0; i < s->held_key_count; ++i)
            otg_bridge_key(&s->bridge, s->held_keys[i].id, s->held_keys[i].down);
        s->held_key_count = 0;
    }
}

static void clipboard_key(app_state *s, uint32_t keyid, bool down)
{
    if (!s->clipboard_error && df_client_clipboard_enabled(s->client) && (s->held_key_count ||
        otg_clipboard_channel_busy(&s->scrcpy.clipboard))) {
        if (s->held_key_count == 64) { clipboard_failed(s); return; }
        s->held_keys[s->held_key_count].id = keyid;
        s->held_keys[s->held_key_count++].down = down;
        return;
    }
    otg_bridge_key(&s->bridge, keyid, down);
}

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
    if (s->unlock_adb_gate && s->display) {
        s->unlock_lock_query_pending = true;
        otg_display_monitor_request_lock(s->display);
    } else if (s->unlock_adb_gate) {
        fprintf(stderr, "WARN: ADB lock-state monitor is unavailable; automatic unlock skipped\n");
    }
}

static void cb_leave(void *ud)
{
    app_state *s = ud;
    clipboard_poll(s);
    if (s->held_key_count) otg_bridge_release_keyboard(&s->bridge);
    s->held_key_count = 0;
    otg_bridge_leave(&s->bridge);
}

static void cb_key_down(void *ud, uint32_t keyid, uint16_t mask)
{
    (void)mask;
    app_state *s = ud;
    clipboard_key(s, keyid, true);
}

static void cb_key_up(void *ud, uint32_t keyid, uint16_t mask)
{
    (void)mask;
    app_state *s = ud;
    clipboard_key(s, keyid, false);
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
    clipboard_poll(s);
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

/* Clipboard uses a short tick interval so that ADB control traffic does not
 * delay Deskflow events.  Do not reuse cb_idle here: its USB transport check
 * enumerates the device list, which is far too expensive to run every 5 ms on
 * Windows and makes pointer motion visibly stutter. */
static int cb_clipboard_tick(void *ud)
{
    app_state *s = ud;
    clipboard_poll(s);
    return 0;
}

static int display_poll(app_state *s)
{
    struct otg_display_size size;
    if (otg_display_monitor_poll(s->display, &size)) {
        s->display_size = size;
        s->display_pending = true;
    }
    if (!s->display_pending || !s->client) return 0;
    size = s->display_size;
    int result = df_client_resize(s->client, size.virtual_width, size.virtual_height);
    if (result < 0) return 1;
    if (result > 0) return 0;
    (void)otg_bridge_resize(&s->bridge, size.virtual_width, size.virtual_height);
    s->display_pending = false;
    fprintf(stderr, "INFO: display size updated: %dx%d (virtual %dx%d); sessions retained\n",
            size.width, size.height, size.virtual_width, size.virtual_height);
    if (g_gui) {
        printf("{\"event\":\"display-size\",\"width\":%d,\"height\":%d,\"virtual-width\":%d,\"virtual-height\":%d}\n",
               size.width, size.height, size.virtual_width, size.virtual_height);
        fflush(stdout);
    }
    return 0;
}

static int cb_tick(void *ud)
{
    app_state *s = ud;
    int result = s->transport_owner ? cb_idle(s) : cb_clipboard_tick(s);
    otg_bridge_unlock_tick(&s->bridge);
#ifdef OTG_HAVE_AUDIO
    const bool ready = !result && audio_player_ready(s->scrcpy.player);
    if (ready != s->audio_ready_reported) {
        s->audio_ready_reported = ready;
        gui_event(ready ? "audio-ready" : "audio-unavailable");
    }
#endif
    if (s->display && display_poll(s)) result = 1;
    if (s->display && s->unlock_lock_query_pending) {
        bool valid = false, locked = false;
        if (otg_display_monitor_take_lock_state(s->display, &valid, &locked)) {
            s->unlock_lock_query_pending = false;
            if (!valid) {
                fprintf(stderr, "WARN: ADB lock-state query returned no recognized state; automatic unlock skipped\n");
            } else {
                fprintf(stderr, "INFO: ADB lock state: %s\n", locked ? "locked" : "unlocked");
            }
            otg_bridge_unlock_adb_state(&s->bridge, valid, locked);
        }
    }
    return result;
}

static void destroy_transport_owner(app_state *s)
{
#ifdef OTG_HAVE_BLE
    if (s->transport_owner) {
        if(s->resident_session)otg_ble_session_destroy(s->transport_owner);
        else otg_ble_hid_destroy(s->transport_owner);
        s->transport_owner = NULL;
    }
#elif defined(OTG_HAVE_BLE_WIN)
    if (s->transport_owner) {
        otg_ble_win_destroy(s->transport_owner);
        s->transport_owner = NULL;
    }
#ifdef _WIN32
    windows_ble_background_cleanup();
#endif
#else
    (void)s;
#endif
}

#if defined(OTG_HAVE_BLE) || defined(OTG_HAVE_BLE_WIN)
static app_state *ble_app;
static int ble_stop_check(void)
{
    if (g_stop || (g_gui && gui_stop_check())) return 1;
#ifdef _WIN32
    if (g_ble_stop_event && WaitForSingleObject(g_ble_stop_event, 0) == WAIT_OBJECT_0) {
        g_stop = 1;
        return 1;
    }
#endif
    if (!ble_app || !otg_bridge_check(&ble_app->bridge)) return 1;
#ifdef OTG_HAVE_BLE_WIN
    /* The phone was ready before TCP connect; losing it during the handshake
     * must cancel partial reads too, not only an already connected session. */
    return !otg_bridge_ready(&ble_app->bridge);
#else
    return ble_app->input_ready_reported && !otg_bridge_ready(&ble_app->bridge);
#endif
}

static bool ble_wait(app_state *s, bool require_ready, unsigned delay_ms)
{
#ifdef _WIN32
    ULONGLONG until = GetTickCount64() + delay_ms;
#else
    gint64 until=g_get_monotonic_time()+(gint64)delay_ms*1000;
#endif
    while (!g_stop && !(g_gui && gui_stop_check())) {
        if (!otg_bridge_check(&s->bridge)) return false;
        if (require_ready && otg_bridge_ready(&s->bridge)) return true;
#ifdef _WIN32
        if (!require_ready && GetTickCount64() >= until) return true;
        Sleep(5);
#else
        if (!require_ready && g_get_monotonic_time()>=until) return true;
        g_usleep(5000);
#endif
    }
    return false;
}
#endif

#ifdef OTG_HAVE_BLE_WIN
/* This runs only after the TCP client has stopped. No input can overtake the
 * release barrier, and no new session is accepted before ble-idle is emitted. */
static bool windows_ble_control_wait(app_state *s)
{
    gui_stop_check();
    if (g_stop) return false;
    if (g_control.phase == OTG_BLE_PAUSING) {
        otg_ble_win_release_inputs(s->transport_owner);
        otg_bridge_reset_input(&s->bridge);
    }
    while (!g_stop && g_control.phase != OTG_BLE_ACTIVE) {
        gui_stop_check();
        if (!otg_bridge_check(&s->bridge)) return false;
        if (g_control.phase == OTG_BLE_PAUSING && otg_ble_win_input_idle(s->transport_owner)) {
            g_control.phase = OTG_BLE_IDLE;
            gui_event("ble-idle");
            fprintf(stderr, "INFO: Windows BLE session detached; GATT provider and subscriptions retained\n");
        }
        Sleep(5);
    }
    return !g_stop;
}
#endif

int main(int argc, char **argv)
{
    struct app_options opts = {
        .host = DEFAULT_HOST,
        .port = DEFAULT_PORT,
        .name = DEFAULT_NAME,
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT,
        .transport = "usb",
        .adapter = "hci0",
        .mouse_mode = OTG_MOUSE_ABSOLUTE,
        .middle_button_action = OTG_MIDDLE_BUTTON_HOME,
        .right_button_action = OTG_RIGHT_BUTTON_BACK,
        .adb_path = "adb",
    };
    char unlock_pin_file_data[64] = {0};

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
            opts.host = argv[++i];
        } else if ((strcmp(a, "-p") == 0 || strcmp(a, "--port") == 0) && i + 1 < argc) {
            opts.port = (unsigned)atoi(argv[++i]);
        } else if ((strcmp(a, "-n") == 0 || strcmp(a, "--name") == 0) && i + 1 < argc) {
            opts.name = argv[++i];
        } else if ((strcmp(a, "-s") == 0 || strcmp(a, "--serial") == 0) && i + 1 < argc) {
            opts.serial = argv[++i];
        } else if (strcmp(a, "--transport") == 0 && i + 1 < argc) {
            opts.transport = argv[++i];
        } else if (strcmp(a, "--adapter") == 0 && i + 1 < argc) {
            opts.adapter = argv[++i];
#ifdef OTG_HAVE_BLE_WIN
            opts.adapter_explicit = true;
#endif
        } else if (strcmp(a, "--ble-isolate") == 0) {
            opts.ble_isolate = 1;
        } else if (strcmp(a, "--ble-restore") == 0) {
            opts.ble_restore = 1;
        } else if (strcmp(a, "--ble-service") == 0) {
            opts.ble_service = 1;
        } else if (strcmp(a, "--ble-session") == 0) {
            opts.ble_session = 1;
        } else if (strcmp(a, "--ble-status") == 0) {
            opts.ble_status = 1;
        } else if (strcmp(a, "--ble-stop-service") == 0) {
            opts.ble_stop_service = 1;
        } else if (strcmp(a, "--ble-peer") == 0 && i + 1 < argc) {
            opts.ble_peer = argv[++i];
#ifdef _WIN32
        } else if (strcmp(a, "--background") == 0) {
            opts.background = 1;
        } else if (strcmp(a, "--background-child") == 0) {
            opts.background = 1;
            opts.background_child = 1;
        } else if (strcmp(a, "--background-stop") == 0) {
            opts.background_stop = 1;
#endif
        } else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
            opts.width = parse_screen_size(argv[++i]);
        } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
            opts.height = parse_screen_size(argv[++i]);
        } else if (strcmp(a, "--gui") == 0) {
            g_gui = 1;
#ifdef OTG_HAVE_BLE_WIN
        } else if (strcmp(a, "--gui-ble-control") == 0) {
            g_ble_control = 1;
#endif
        } else if (strcmp(a, "--mouse-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "absolute") == 0) {
                opts.mouse_mode = OTG_MOUSE_ABSOLUTE;
            } else if (strcmp(mode, "relative") == 0) {
                opts.mouse_mode = OTG_MOUSE_RELATIVE;
            } else {
                fprintf(stderr, "invalid mouse mode: %s (use absolute or relative)\n", mode);
                return 2;
            }
        } else if (strcmp(a, "--right-button-action") == 0 && i + 1 < argc) {
            const char *action = argv[++i];
            if (strcmp(action, "normal") == 0) {
                opts.right_button_action = OTG_RIGHT_BUTTON_NORMAL;
            } else if (strcmp(action, "back") == 0) {
                opts.right_button_action = OTG_RIGHT_BUTTON_BACK;
            } else {
                fprintf(stderr, "invalid right button action: %s (use normal or back)\n", action);
                return 2;
            }
        } else if (strcmp(a, "--middle-button-action") == 0 && i + 1 < argc) {
            const char *action = argv[++i];
            if (strcmp(action, "normal") == 0) {
                opts.middle_button_action = OTG_MIDDLE_BUTTON_NORMAL;
            } else if (strcmp(action, "home") == 0) {
                opts.middle_button_action = OTG_MIDDLE_BUTTON_HOME;
            } else {
                fprintf(stderr, "invalid middle button action: %s (use normal or home)\n", action);
                return 2;
            }
        } else if (strcmp(a, "--audio") == 0) {
            opts.audio = 1;
        } else if (strcmp(a, "--clipboard") == 0) {
            opts.clipboard = 1;
        } else if (strcmp(a, "--tls") == 0) {
            opts.tls.enabled = true;
        } else if (strcmp(a, "--tls-cert") == 0 && i + 1 < argc) {
            opts.tls.certificate = argv[++i];
        } else if (strcmp(a, "--tls-fingerprint") == 0 && i + 1 < argc) {
            opts.tls.fingerprint = argv[++i];
        } else if (strcmp(a, "--tls-fingerprint-file") == 0 && i + 1 < argc) {
            opts.tls.fingerprint_file = argv[++i];
        } else if (strcmp(a, "--auto-rotate") == 0) {
            opts.auto_rotate = 1;
        } else if (strcmp(a, "--adb") == 0 && i + 1 < argc) {
            opts.adb_path = argv[++i];
        } else if (strcmp(a, "--adb-serial") == 0 && i + 1 < argc) {
            opts.adb_serial = argv[++i];
        } else if (strcmp(a, "--unlock-pin-file") == 0 && i + 1 < argc) {
            /* Internal GUI hand-off for pkexec: the PIN must not be put in
             * argv, while the file path survives the privilege boundary. */
            opts.unlock_pin_file = argv[++i];
        } else if (strcmp(a, "--scrcpy-server") == 0 && i + 1 < argc) {
            opts.scrcpy_server = argv[++i];
        } else if (strcmp(a, "--scrcpy-version") == 0 && i + 1 < argc) {
            opts.scrcpy_version = argv[++i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!opts.width || !opts.height) {
        fprintf(stderr, "screen width and height must be integers in 1..32767\n");
        return 2;
    }
    opts.unlock_pin = getenv("OTG_UNLOCK_PIN");
    opts.unlock_adb_gate = getenv("OTG_UNLOCK_ADB_GATE") &&
                           strcmp(getenv("OTG_UNLOCK_ADB_GATE"), "1") == 0;
    if (opts.unlock_pin_file) {
        FILE *file = fopen(opts.unlock_pin_file, "rb");
        if (!file) {
            fprintf(stderr, "ERROR: cannot read unlock PIN file: %s\n", opts.unlock_pin_file);
            return 2;
        }
        const size_t length = fread(unlock_pin_file_data, 1,
                                    sizeof(unlock_pin_file_data) - 1, file);
        const bool read_error = ferror(file) != 0;
        const bool too_long = !feof(file);
        fclose(file);
        if (read_error || too_long) {
            fprintf(stderr, "ERROR: invalid unlock PIN file\n");
            return 2;
        }
        unlock_pin_file_data[length] = '\0';
        if (!unlock_pin_file_data[0]) {
            fprintf(stderr, "ERROR: unlock PIN file is empty\n");
            return 2;
        }
        opts.unlock_pin = unlock_pin_file_data;
        opts.unlock_adb_gate = 1;
    }
    /* ADB is an auxiliary dependency, not part of the HID transport.  Keep
     * this single predicate in sync with the options that actually invoke
     * ADB, so feature-only sessions get the same validation and lifecycle. */
    const int adb_features = opts.audio || opts.clipboard || opts.unlock_adb_gate;
    if (opts.unlock_pin && *opts.unlock_pin &&
        (strlen(opts.unlock_pin) < 4 || strlen(opts.unlock_pin) > 32)) {
        fprintf(stderr, "ERROR: OTG_UNLOCK_PIN must contain 4..32 digits\n");
        return 2;
    }
    if (opts.unlock_pin && *opts.unlock_pin) {
        for (const char *p = opts.unlock_pin; *p; ++p) {
            if (*p < '0' || *p > '9') {
                fprintf(stderr, "ERROR: OTG_UNLOCK_PIN must contain digits only\n");
                return 2;
            }
        }
    }
    if (adb_features && (opts.ble_service || opts.ble_status || opts.ble_stop_service || opts.ble_restore)) {
        fprintf(stderr, "ERROR: ADB features require a bridge session, not standalone BLE service operations\n");
        return 2;
    }
    if (adb_features && (!(opts.adb_serial && *opts.adb_serial) && !(opts.serial && *opts.serial))) {
        fprintf(stderr, "ERROR: ADB features require --adb-serial or --serial\n");
        return 2;
    }
    if (opts.auto_rotate && !(opts.audio || opts.clipboard)) {
        fprintf(stderr, "ERROR: --auto-rotate requires --audio or --clipboard\n");
        return 2;
    }
    if (!opts.tls.enabled && (opts.tls.certificate || opts.tls.fingerprint || opts.tls.fingerprint_file)) {
        fprintf(stderr, "ERROR: TLS options require --tls\n");
        return 2;
    }
    if (opts.tls.enabled && ((!opts.tls.fingerprint || !*opts.tls.fingerprint) &&
                            (!opts.tls.fingerprint_file || !*opts.tls.fingerprint_file))) {
        fprintf(stderr, "ERROR: --tls requires --tls-fingerprint or --tls-fingerprint-file\n");
        return 2;
    }
    if (opts.tls.enabled && opts.tls.fingerprint && opts.tls.fingerprint_file) {
        fprintf(stderr, "ERROR: use either --tls-fingerprint or --tls-fingerprint-file, not both\n");
        return 2;
    }
    if (opts.tls.enabled && (opts.ble_service || opts.ble_status || opts.ble_stop_service || opts.ble_restore)) {
        fprintf(stderr, "ERROR: TLS is only available for a Deskflow bridge connection\n");
        return 2;
    }
    if ((opts.audio || opts.clipboard) && (!opts.scrcpy_server || !*opts.scrcpy_server ||
                                         !opts.scrcpy_version || !*opts.scrcpy_version)) {
        fprintf(stderr, "ERROR: ADB features require --scrcpy-server and --scrcpy-version\n");
        return 2;
    }
    if (opts.adb_serial && !adb_features) {
        fprintf(stderr, "ERROR: --adb-serial requires an ADB feature (audio, clipboard or unlock)\n");
        return 2;
    }
#ifndef OTG_HAVE_AUDIO
    if (opts.audio) {
        fprintf(stderr, "ERROR: audio support was not built; configure with -DBUILD_AUDIO=ON\n");
        return 2;
    }
#endif
#ifndef OTG_HAVE_CLIPBOARD
    if (opts.clipboard) {
        fprintf(stderr, "ERROR: clipboard support was not built; configure with -DBUILD_CLIPBOARD=ON\n");
        return 2;
    }
#endif

#ifdef _WIN32
    if (opts.background_stop) {
        if (argc != 2) {
            fprintf(stderr, "ERROR: --background-stop takes no other arguments\n");
            return 2;
        }
        return windows_ble_stop_background();
    }
#endif

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(on_console_ctrl, TRUE)) {
        fprintf(stderr, "WARN: could not install Windows console control handler\n");
    }
#endif
#ifndef _WIN32
    /* This must precede the supervisor dispatch too: losing the GUI's log
     * pipes must not kill the root process before it restores adapter modes. */
    if(g_gui)signal(SIGPIPE,SIG_IGN);
#endif
    if(strcmp(opts.transport,"usb") && strcmp(opts.transport,"ble")) {
        fprintf(stderr,"ERROR: invalid transport (use usb or ble)\n");return 2;
    }
    if(!strcmp(opts.transport,"ble") || opts.ble_service || opts.ble_session || opts.ble_status || opts.ble_stop_service || opts.ble_restore) {
        char *end=NULL;
        unsigned long index=strncmp(opts.adapter,"hci",3) ? UINT16_MAX : strtoul(opts.adapter+3,&end,10);
        if(!end || end==opts.adapter+3 || *end || opts.adapter[3]<'0' || opts.adapter[3]>'9' || index>=UINT16_MAX) {
            fprintf(stderr,"ERROR: invalid adapter (use hci0..hci65534)\n");return 2;
        }
    }
    if(!strcmp(opts.transport,"ble") && !opts.ble_service && !opts.ble_status && !opts.ble_stop_service && !opts.ble_restore && !opts.ble_peer) {
        fprintf(stderr,"ERROR: BLE bridge requires --ble-peer identity-address\n");return 2;
    }
    if(opts.ble_peer) {
        bool valid=strlen(opts.ble_peer)==17;
        for(unsigned i=0;valid && i<17;i++) {
            char c=opts.ble_peer[i];
            valid=i%3==2 ? c==':' : (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
        }
        if(!valid || strcmp(opts.transport,"ble") || opts.ble_service || opts.ble_status || opts.ble_stop_service || opts.ble_restore) {
            fprintf(stderr,"ERROR: --ble-peer requires BLE bridge mode and an identity MAC address\n");return 2;
        }
    }
#ifdef OTG_HAVE_BLE_WIN
    if (g_ble_control && (!g_gui || strcmp(opts.transport, "ble") || opts.background ||
        strlen(opts.host) >= sizeof(g_control.host) || strlen(opts.name) >= sizeof(g_control.name))) {
        fprintf(stderr, "ERROR: --gui-ble-control requires foreground --gui --transport ble\n");
        return 2;
    }
    if (g_ble_control) {
        strcpy(g_control.host, opts.host); strcpy(g_control.name, opts.name);
        g_control.port = opts.port; g_control.width = (unsigned)opts.width; g_control.height = (unsigned)opts.height;
        g_control.middle_button_home = opts.middle_button_action == OTG_MIDDLE_BUTTON_HOME;
        g_control.right_button_back = opts.right_button_action == OTG_RIGHT_BUTTON_BACK;
    }
    if (!strcmp(opts.transport, "ble") && !opts.ble_service && !opts.ble_status && !opts.ble_stop_service && !opts.ble_restore &&
        (opts.ble_session || opts.ble_isolate || opts.serial || (opts.adb_serial && !(opts.clipboard || opts.unlock_adb_gate)) ||
         opts.adapter_explicit || opts.mouse_mode != OTG_MOUSE_ABSOLUTE)) {
        fprintf(stderr, "ERROR: Windows BLE supports default adapter and absolute pointer only; Linux service/isolation options and USB serial are unsupported\n");
        return 2;
    }
#ifdef _WIN32
    if (opts.background && g_gui) {
        fprintf(stderr, "ERROR: --background cannot be combined with --gui (GUI owns the bridge lifetime)\n");
        return 2;
    }
    if (opts.background && strcmp(opts.transport, "ble") != 0) {
        fprintf(stderr, "ERROR: --background is supported only with Windows BLE\n");
        return 2;
    }
    if (opts.background && !opts.background_child) {
        return windows_ble_start_background_child();
    }
    if (opts.background_child && !windows_ble_background_init()) {
        return 1;
    }
#endif
#endif
    if(opts.ble_status || opts.ble_stop_service) {
        if((opts.ble_status && opts.ble_stop_service) || opts.ble_service || opts.ble_session || opts.ble_isolate || opts.ble_restore || g_gui || opts.serial || opts.adb_serial)return 2;
#ifdef OTG_HAVE_BLE
        return opts.ble_status ? otg_ble_service_status(opts.adapter) : otg_ble_service_stop(opts.adapter);
#else
        fprintf(stderr,"ERROR: BLE requires BUILD_BLE=ON\n");return 2;
#endif
    }
    if(opts.ble_service || opts.ble_session) {
        if((opts.ble_service && (g_gui || opts.ble_session || opts.ble_isolate || opts.ble_restore || opts.serial || opts.adb_serial)) ||
           (opts.ble_session && (!opts.ble_peer || opts.ble_restore || strcmp(opts.transport,"ble") || opts.mouse_mode!=OTG_MOUSE_ABSOLUTE))) {
            fprintf(stderr,"ERROR: --ble-service is standalone; --ble-session requires --transport ble, --ble-peer and absolute mouse mode\n");return 2;
        }
#ifdef OTG_HAVE_BLE
        if(opts.ble_service)return otg_ble_service_run(opts.adapter,&g_stop);
#else
        fprintf(stderr,"ERROR: resident BLE requires BUILD_BLE=ON\n");return 2;
#endif
    }
    if (opts.ble_isolate || opts.ble_restore) {
        if ((g_gui && opts.ble_restore) || (opts.ble_isolate && opts.ble_restore) ||
            (opts.ble_isolate && strcmp(opts.transport, "ble"))) {
            fprintf(stderr, "ERROR: use --ble-isolate with --transport ble in CLI; --ble-restore is separate\n");
            return 2;
        }
#ifdef OTG_HAVE_BLE
        return otg_ble_supervise(argc, argv, opts.adapter, opts.ble_restore, &g_stop);
#else
        fprintf(stderr, "ERROR: BLE supervision requires Linux with BUILD_BLE=ON (adapter %s)\n", opts.adapter);
        return 2;
#endif
    }
    if (g_gui) {
        df_client_set_stop_check(gui_stop_check);
        if (gui_stop_check() && g_stop) {
            return 0;
        }
    }

    app_state state;
    memset(&state, 0, sizeof(state));
    state.serial = opts.serial;

    /* 1. Open the selected HID transport and register keyboard + pointer. */
    if (strcmp(opts.transport, "usb") == 0) {
        if (otg_bridge_open(&state.bridge, opts.serial, opts.width, opts.height, opts.mouse_mode) != 0) {
            return 1;
        }
        otg_bridge_set_middle_button_action(&state.bridge, opts.middle_button_action);
        otg_bridge_set_right_button_action(&state.bridge, opts.right_button_action);
        fprintf(stderr, "INFO:  AOA HID keyboard + %s pointer ready (screen %dx%d)\n",
                opts.mouse_mode == OTG_MOUSE_ABSOLUTE ? "absolute" : "relative", opts.width, opts.height);
    } else if (strcmp(opts.transport, "ble") == 0) {
#ifdef OTG_HAVE_BLE
        state.resident_session=opts.ble_session;
        state.transport_owner=opts.ble_session ? (void *)otg_ble_session_open(opts.adapter) :
                                                 (void *)otg_ble_hid_create(opts.adapter);
        if(state.transport_owner && opts.ble_peer &&
           !(opts.ble_session ? otg_ble_session_set_peer(state.transport_owner,opts.ble_peer) :
                                otg_ble_hid_set_peer(state.transport_owner,opts.ble_peer))) {
            fprintf(stderr,"ERROR: BLE target differs from resident identity; explicitly stop the idle service before changing phones\n");
            destroy_transport_owner(&state);return 1;
        }
        const struct otg_hid_transport_ops *ops=opts.ble_session ? otg_ble_session_ops() : otg_ble_hid_ops();
        if (!state.transport_owner || otg_bridge_open_transport(&state.bridge, ops, state.transport_owner,
                                              opts.width, opts.height, opts.mouse_mode) != 0) {
            destroy_transport_owner(&state);
            return 1;
        }
        otg_bridge_set_middle_button_action(&state.bridge, opts.middle_button_action);
        otg_bridge_set_right_button_action(&state.bridge, opts.right_button_action);
        fprintf(stderr, "INFO:  BLE HID advertisement started on %s; waiting for phone subscriptions\n", opts.adapter);
#elif defined(OTG_HAVE_BLE_WIN)
        state.transport_owner = otg_ble_win_create(opts.ble_peer);
        if (!state.transport_owner || otg_bridge_open_transport(&state.bridge,
                otg_ble_win_ops(), state.transport_owner, opts.width, opts.height, opts.mouse_mode) != 0) {
            destroy_transport_owner(&state);
            return 1;
        }
        otg_bridge_set_middle_button_action(&state.bridge, opts.middle_button_action);
        otg_bridge_set_right_button_action(&state.bridge, opts.right_button_action);
        fprintf(stderr, "INFO: Windows BLE target %s; starting GATT worker, waiting for paired phone subscriptions\n", opts.ble_peer);
#else
        fprintf(stderr, "ERROR: BLE HID requires Linux with BUILD_BLE=ON (adapter %s)\n", opts.adapter);
        return 2;
#endif
    } else {
        fprintf(stderr, "invalid transport: %s (use usb or ble)\n", opts.transport);
        return 2;
    }
    if (!otg_bridge_set_unlock_pin(&state.bridge, opts.unlock_pin)) {
        fprintf(stderr, "ERROR: invalid OTG_UNLOCK_PIN\n");
        otg_bridge_close(&state.bridge);
        destroy_transport_owner(&state);
        return 2;
    }
    state.unlock_adb_gate = opts.unlock_adb_gate != 0;
    otg_bridge_set_unlock_adb_gated(&state.bridge, state.unlock_adb_gate);
    if (g_gui && strcmp(opts.transport, "usb") == 0) {
        printf("{\"event\":\"usb-ready\",\"usb-id\":\"%u:%u\"}\n",
               (unsigned)libusb_get_bus_number(state.bridge.aoa.device),
               (unsigned)libusb_get_device_address(state.bridge.aoa.device));
        fflush(stdout);
    }
    if (g_gui && gui_stop_check() && g_stop) {
          scrcpy_session_stop(&state.scrcpy);
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
        .on_tick = state.transport_owner || adb_features ? cb_tick : NULL,
        .tick_interval_ms = state.transport_owner ? 1 : opts.clipboard ? 5 : opts.audio ? 50 :
                            opts.unlock_adb_gate ? 5 : 0,
        .on_clipboard = cb_clipboard,
        .on_clipboard_enabled = opts.clipboard ? cb_clipboard_enabled : NULL,
    };

    int ret = 0;
#if defined(OTG_HAVE_BLE) || defined(OTG_HAVE_BLE_WIN)
    if (state.transport_owner) {ble_app=&state;df_client_set_stop_check(ble_stop_check);}
#endif
    do {
#ifdef OTG_HAVE_BLE_WIN
        if (g_ble_control) {
            if (!windows_ble_control_wait(&state)) break;
            opts.host = g_control.host; opts.name = g_control.name; opts.port = g_control.port;
            opts.width = (int)g_control.width; opts.height = (int)g_control.height;
            state.bridge.screen_w = opts.width; state.bridge.screen_h = opts.height;
            otg_bridge_set_middle_button_action(&state.bridge,
                g_control.middle_button_home ? OTG_MIDDLE_BUTTON_HOME : OTG_MIDDLE_BUTTON_NORMAL);
            otg_bridge_set_right_button_action(&state.bridge,
                g_control.right_button_back ? OTG_RIGHT_BUTTON_BACK : OTG_RIGHT_BUTTON_NORMAL);
        }
#endif
#if defined(OTG_HAVE_BLE) || defined(OTG_HAVE_BLE_WIN)
        if (state.transport_owner && !ble_wait(&state,true,0)) {
#ifdef OTG_HAVE_BLE_WIN
            if (g_ble_control && !g_stop && g_control.phase != OTG_BLE_ACTIVE) continue;
#endif
            if (!g_stop) ret = 1;
            break;
        }
#endif
        state.clipboard_error = false;
        state.audio_ready_reported = false;
        if ((opts.audio || opts.clipboard) && scrcpy_session_start(&state.scrcpy, opts.adb_path,
                opts.adb_serial ? opts.adb_serial : opts.serial, opts.scrcpy_server, opts.scrcpy_version,
                opts.audio != 0, opts.clipboard != 0)) {
            if (!opts.clipboard) { ret = 1; break; } /* Preserve audio-only startup semantics. */
            clipboard_failed(&state);
        }
        if ((opts.auto_rotate && scrcpy_session_started(&state.scrcpy)) || state.unlock_adb_gate) {
            const char *target = opts.adb_serial && *opts.adb_serial ? opts.adb_serial : opts.serial;
            state.display = otg_display_monitor_create(opts.adb_path, target,
                                                       state.bridge.screen_w, state.bridge.screen_h);
            if (state.display && state.unlock_adb_gate)
                otg_display_monitor_prefer_companion(state.display);
            if (!state.display && opts.auto_rotate)
                fprintf(stderr, "WARN: automatic display size monitoring unavailable\n");
            if (!state.display && state.unlock_adb_gate) {
                fprintf(stderr, "WARN: ADB lock-state monitoring unavailable; automatic unlock disabled\n");
                state.unlock_adb_gate = false;
                otg_bridge_set_unlock_adb_gated(&state.bridge, false);
                (void)otg_bridge_set_unlock_pin(&state.bridge, NULL);
            }
        }
        state.display_pending = false;
        state.unlock_lock_query_pending = false;
        df_client *client=df_client_connect_ex(opts.host,(uint16_t)opts.port,opts.name,state.bridge.screen_w,state.bridge.screen_h,
                                               &cbs,&state,&opts.tls);
        if(client) {
            state.client = client;
            df_client_enable_clipboard(client, opts.clipboard && !state.clipboard_error);
            fprintf(stderr,"INFO: connected to deskflow server %s:%u as \"%s\"\n",opts.host,opts.port,opts.name);
            ret=df_client_run(client,&g_stop);
            state.client = NULL;
            state.held_key_count = 0;
            df_client_disconnect(client);
        } else ret=1;
        otg_display_monitor_destroy(state.display); state.display = NULL;
        state.display_pending = false;
        scrcpy_session_stop(&state.scrcpy);
        if(!state.transport_owner || g_stop) break;
#ifdef OTG_HAVE_BLE_WIN
        if (g_ble_control && g_control.phase != OTG_BLE_ACTIVE) {
            /* Do not enqueue leave/reset into a nearly full old queue. The
             * next iteration discards it before adding release barriers. */
            state.deskflow_connected = state.input_ready_reported = false;
            continue;
        }
#endif
        otg_bridge_leave(&state.bridge);
        otg_bridge_reset_input(&state.bridge);
        state.deskflow_connected=state.input_ready_reported=false;
        gui_event("disconnected");
        fprintf(stderr,"INFO: BLE service retained; reconnect phone without removing pairing; retrying Deskflow when ready\n");
#if defined(OTG_HAVE_BLE) || defined(OTG_HAVE_BLE_WIN)
        if(!ble_wait(&state,false,1000)) {
#ifdef OTG_HAVE_BLE_WIN
            if (g_ble_control && !g_stop && g_control.phase != OTG_BLE_ACTIVE) continue;
#endif
            if (!g_stop) ret = 1;
            break;
        }
#endif
    } while(!g_stop);
    scrcpy_session_stop(&state.scrcpy);
    otg_bridge_close(&state.bridge);
    destroy_transport_owner(&state);
    gui_event("disconnected");
    (void)g_stop;
    return ret;
}
