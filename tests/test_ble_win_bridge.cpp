/* Actual C bridge -> Windows queue/map, with no USB or Bluetooth access.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_win_queue.h"
extern "C" {
#include "bridge.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); std::exit(1); } } while (0)
using namespace otg_ble_win_detail;

// These must never be used by an external-transport bridge.
extern "C" bool aoa_hid_init(aoa_hid *) { std::abort(); }
extern "C" bool aoa_hid_open(aoa_hid *, const char *) { std::abort(); }
extern "C" void aoa_hid_destroy(aoa_hid *) { std::abort(); }
extern "C" bool aoa_hid_register(aoa_hid *, uint16_t, const uint8_t *, uint16_t) { std::abort(); }
extern "C" bool aoa_hid_send(aoa_hid *, uint16_t, const uint8_t *, uint16_t) { std::abort(); }
extern "C" bool aoa_hid_poll(aoa_hid *) { std::abort(); }
extern "C" bool aoa_hid_is_disconnected(const aoa_hid *) { std::abort(); }

struct Fake {
    Queue queue;
    std::vector<uint8_t> map;
    unsigned registered = 0, closed = 0;
    bool ready = false;
};
static bool register_report(void *ctx, uint16_t id, const uint8_t *desc, uint16_t size)
{
    auto& f = *static_cast<Fake *>(ctx);
    CHECK(id == ++f.registered);
    CHECK(id != 2 || (desc[1] == 0x0d && desc[3] == 1));
    return append_descriptor(f.map, desc, size, static_cast<uint8_t>(id));
}
static bool send(void *ctx, uint16_t id, const uint8_t *data, uint16_t size)
{
    auto& f = *static_cast<Fake *>(ctx);
    CHECK(f.ready && size == 8);
    Report report{id, {}, false, 1};
    std::memcpy(report.bytes.data(), data, 8);
    return f.queue.push(report);
}
static bool poll(void *) { return true; }
static bool ready(void *ctx) { return static_cast<Fake *>(ctx)->ready; }
static void close(void *ctx) { ++static_cast<Fake *>(ctx)->closed; }

int main()
{
    Fake f;
    otg_bridge bridge;
    const otg_hid_transport_ops ops = {register_report, send, poll, ready, close};
    CHECK(otg_bridge_open_transport(&bridge, &ops, &f, 1080, 2340, OTG_MOUSE_ABSOLUTE) == 0);
    CHECK(f.registered == 2 && f.map[6] == 0x85 && f.map[7] == 1);
    CHECK(!otg_bridge_ready(&bridge));
    otg_bridge_enter(&bridge, 10, 20, 0);
    otg_bridge_key(&bridge, 'a', true);
    CHECK(f.queue.empty()); // unsubscribed reports never queue for replay
    otg_bridge_reset_input(&bridge);
    f.ready = true;
    otg_bridge_reset_input(&bridge);
    CHECK(f.queue.pop().bytes == (std::array<uint8_t, 8>{}));
    CHECK(f.queue.pop().bytes[0] == 0);

    otg_bridge_enter(&bridge, 0, 0, 0);
    CHECK(f.queue.pop().bytes[0] == 2); // hover, not press
    CHECK(f.queue.pop().id == 1);
    otg_bridge_mouse_move(&bridge, 10, 10);
    otg_bridge_mouse_move(&bridge, 1079, 2339);
    CHECK(f.queue.size() == 1);
    otg_bridge_mouse_button(&bridge, 1, true);
    otg_bridge_key(&bridge, 'a', true);
    otg_bridge_key(&bridge, 'a', false);
    otg_bridge_mouse_wheel(&bridge, 0, 120);
    otg_bridge_leave(&bridge);
    auto move = f.queue.pop();
    CHECK(move.bytes[2] == 0xff && move.bytes[3] == 0x7f &&
        move.bytes[4] == 0xff && move.bytes[5] == 0x7f);
    auto down = f.queue.pop();
    CHECK(down.bytes[0] == 3 && down.bytes[1] == 1 && down.bytes[2] == 0xff);
    CHECK(f.queue.pop().bytes[2] == 4); // a down remains before a up
    CHECK(f.queue.pop().bytes[2] == 0);
    CHECK(f.queue.pop().bytes[6] == 1);
    CHECK(f.queue.pop().bytes[1] == 0); // buttons released before ending range
    CHECK(f.queue.pop().bytes[0] == 0);
    CHECK(f.queue.empty());

    f.ready = false;
    otg_bridge_key(&bridge, 'b', true);
    CHECK(f.queue.empty());
    otg_bridge_reset_input(&bridge);
    f.ready = true;
    otg_bridge_reset_input(&bridge);
    CHECK(f.queue.pop().bytes == (std::array<uint8_t, 8>{}));
    CHECK(f.queue.pop().bytes[0] == 0);
    otg_bridge_close(&bridge);
    otg_bridge_close(&bridge);
    CHECK(f.closed == 1);
    std::puts("PASS: C bridge descriptors, HID encoding, ordered queued input, readiness reset and close");
    return 0;
}
