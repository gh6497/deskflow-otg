/* Real main.c/control/bridge with fake BLE + Deskflow. No sockets, USB or
 * WinRT calls: verifies provider lifetime and pause barriers on Windows.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_win.h"
#include "bridge.h"
#include "deskflow_client.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct otg_ble_win { unsigned registered; ULONGLONG release_at; };
static struct otg_ble_win provider;
static int (*stop_check)(void);
struct df_client { const df_client_callbacks *cbs; void *ud; };
static struct df_client client;
static void trace(const char *text) { fprintf(stderr, "FAKE %s\n", text); fflush(stderr); }

static bool reg(void *ctx, uint16_t id, const uint8_t *desc, uint16_t size)
{
    struct otg_ble_win *b = ctx;
    if (!desc || !size || id != b->registered + 1) return false;
    ++b->registered;
    return true;
}
static bool send_report(void *ctx, uint16_t id, const uint8_t *data, uint16_t size)
{ (void)ctx; (void)id; (void)data; return size == 8; }
static bool poll_report(void *ctx) { (void)ctx; return true; }
static bool ready(void *ctx) { (void)ctx; return !getenv("OTG_TEST_NATIVE_BLE_WAIT"); }
static void close_report(void *ctx) { (void)ctx; }
struct otg_ble_win *otg_ble_win_create(const char *peer)
{ (void)peer; memset(&provider, 0, sizeof(provider)); trace("provider-created"); return &provider; }
const struct otg_hid_transport_ops *otg_ble_win_ops(void)
{
    static const struct otg_hid_transport_ops ops = {reg, send_report, poll_report, ready, close_report};
    return &ops;
}
void otg_ble_win_destroy(struct otg_ble_win *b) { (void)b; trace("provider-closed"); }
void otg_ble_win_release_inputs(struct otg_ble_win *b)
{ b->release_at = GetTickCount64() + 80; trace("release-requested"); }
bool otg_ble_win_input_idle(struct otg_ble_win *b)
{
    if (getenv("OTG_TEST_NATIVE_BLE_RELEASE_STALL")) return false;
    if (GetTickCount64() < b->release_at) return false;
    if (b->release_at) { b->release_at = 0; trace("input-released"); }
    return true;
}
void df_client_set_stop_check(int (*check)(void)) { stop_check = check; }
df_client *df_client_connect(const char *host, uint16_t port, const char *name,
                            int32_t w, int32_t h, const df_client_callbacks *cbs, void *ud)
{
    if (stop_check && stop_check()) return NULL;
    fprintf(stderr, "FAKE session=%s,%u,%s,%d,%d\n", host, port, name, (int)w, (int)h); fflush(stderr);
    client.cbs = cbs; client.ud = ud;
    return &client;
}
int df_client_run(df_client *c, volatile sig_atomic_t *stop)
{
    c->cbs->on_connected(c->ud);
    c->cbs->on_enter(c->ud, 100, 100, 1, 0);
    c->cbs->on_key_down(c->ud, 'a', 0);
    c->cbs->on_mouse_down(c->ud, 1);
    trace("input-held");
    while (!*stop && !(stop_check && stop_check())) Sleep(2);
    return 0;
}
void df_client_disconnect(df_client *c) { (void)c; trace("tcp-closed"); }
