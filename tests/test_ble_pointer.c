/* Offline BLE coalescing tests: no system bus or adapter access.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/ble_hid.c"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

int main(void)
{
    struct otg_ble_hid b = {0};
    b.app_registered = b.notifying[0] = b.notifying[1] = true;
    uint64_t t = UINT64_C(1000000000);
    uint8_t report[8] = {2, 0, 10, 0, 20, 0, 0, 0};
    CHECK(queue_pointer(&b, report, 8, t));
    for (int i = 1; i <= 20; ++i) {
        report[2] = (uint8_t)i;
        CHECK(!queue_pointer(&b, report, 8, t + i * UINT64_C(1000000)));
    }
    CHECK(b.values[1][2] == 20);
    CHECK(!pointer_due(&b, t + BLE_POINTER_INTERVAL_NS - 1));
    CHECK(pointer_due(&b, t + BLE_POINTER_INTERVAL_NS));
    CHECK(!pointer_due(&b, t + BLE_POINTER_INTERVAL_NS + 1));

    t += BLE_POINTER_INTERVAL_NS + 2;
    report[0] = 3;
    report[1] = 1;
    CHECK(queue_pointer(&b, report, 8, t)); /* press is immediate */
    report[2] = 80;
    CHECK(!queue_pointer(&b, report, 8, t + 1)); /* drag coalesces */
    report[0] = 2;
    report[1] = 0;
    CHECK(queue_pointer(&b, report, 8, t + 2)); /* release is immediate */
    CHECK(!b.pointer_pending);
    report[6] = 1;
    CHECK(queue_pointer(&b, report, 8, t + 3)); /* wheel is immediate */
    CHECK(queue_pointer(&b, report, 8, t + 4)); /* repeated wheel preserved */
    report[6] = 0;
    report[0] = 0;
    CHECK(queue_pointer(&b, report, 8, t + 5)); /* range exit immediate */

    report[0] = 2;
    CHECK(queue_pointer(&b, report, 8, t + 6));
    CHECK(!queue_pointer(&b, report, 8, t + 7));
    b.notifying[0] = false;
    CHECK(!pointer_due(&b, t + 2 * BLE_POINTER_INTERVAL_NS));
    CHECK(!b.pointer_pending); /* disconnected input never replayed */

    /* Real unprivileged IPC sockets, but still no Bluetooth hardware access. */
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0);
    b.latency_fd = fds[0];
    b.adapter = "hci0";
    latency_state(&b, true);
    char message[64] = {0};
    CHECK(recv(fds[1], message, sizeof(message), 0) == 11);
    CHECK(strcmp(message, "ready:hci0\n") == 0);
    const char slow[] = "interval_us=45000\n";
    CHECK(send(fds[1], slow, sizeof(slow) - 1, 0) > 0);
    latency_poll(&b);
    CHECK(pointer_interval(&b) == UINT64_C(45000000));
    const char fast[] = "interval_us=7500\n";
    CHECK(send(fds[1], fast, sizeof(fast) - 1, 0) > 0);
    latency_poll(&b);
    CHECK(pointer_interval(&b) == UINT64_C(16666667));
    const char invalid[] = "interval_us=0\n";
    CHECK(send(fds[1], invalid, sizeof(invalid) - 1, 0) > 0);
    latency_poll(&b);
    CHECK(pointer_interval(&b) == UINT64_C(16666667));
    latency_state(&b, false);
    CHECK(pointer_interval(&b) == BLE_POINTER_INTERVAL_NS);
    close(fds[0]);
    close(fds[1]);
    b.latency_fd = -1;
    b.peer_connected = true;
    b.notifying[0] = b.notifying[1] = true;
    update_channel_state(&b);
    CHECK(ready_ble(&b));
    b.values[0][2] = 4; /* Held key and button from the previous connection. */
    b.values[1][1] = 1;
    b.pointer_pending = true;
    b.peer_connected = false;
    update_channel_state(&b);
    CHECK(!ready_ble(&b));
    CHECK(b.notifying[0] && b.notifying[1]); /* Bonded BlueZ CCCs are retained. */
    CHECK(!b.values[0][2] && !b.values[1][1] && !b.pointer_pending);
    b.peer_connected = true;
    update_channel_state(&b);
    CHECK(ready_ble(&b)); /* No second StartNotify needed on cached reconnect. */

    struct ble_object map={&b,BLE_MAP,0};
    struct ble_object keys={&b,BLE_REPORT,0};
    struct ble_object pointer={&b,BLE_REPORT,1};
    struct ble_object led_ref={&b,BLE_REF,2};
    CHECK(object_handle(&map)==0x102);
    CHECK(object_handle(&keys)==0x106 && object_handle(&pointer)==0x10a);
    CHECK(object_handle(&led_ref)==0x10f);
    CHECK(!strcmp(path_for(&led_ref),ROOT "/hid/leds/reference"));
    GVariant *properties=g_variant_ref_sink(props(&keys,iface_for(BLE_REPORT)));
    guint16 handle=0;
    CHECK(g_variant_lookup(properties,"Handle","q",&handle) && handle==0x106);
    g_variant_unref(properties);
    puts("PASS: latest coordinates, drag, release, wheel, range and cancellation");
    return 0;
}
