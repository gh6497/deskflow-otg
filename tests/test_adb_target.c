/* Shared ADB startup policy tests. No ADB server or devices.
 * SPDX-License-Identifier: Apache-2.0 */
#include "adb_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

struct fake { unsigned states, connects; bool ready, fail_connect, stay_offline; };
static int command(void *ud, const char *target, bool connect)
{
    struct fake *f = ud;
    CHECK(!strcmp(target, "192.0.2.1:40195"));
    if (connect) {
        ++f->connects;
        if (f->fail_connect) return -1;
        f->ready = !f->stay_offline;
        return 0;
    }
    ++f->states;
    return f->ready ? 0 : -1;
}

int main(void)
{
    const char *tcp[] = {"192.0.2.1:40195", "phone.local:5555", "localhost:1",
                         "[::1]:5555", "[fe80::1%wlan0]:65535"};
    const char *other[] = {NULL, "", "bb6dd386", "emulator-5554",
        "adb-phone._adb-tls-connect._tcp", "192.0.2.1", "phone:0", "phone:65536",
        "phone:-1", "phone:", ":5555", "phone:99999999999999999",
        "phone:5555 extra", "phone\":5555", "-phone:5555", "[::1]", "[::1]:bad"};
    for (unsigned i = 0; i < sizeof(tcp) / sizeof(*tcp); ++i) CHECK(otg_adb_target_is_tcp(tcp[i]));
    for (unsigned i = 0; i < sizeof(other) / sizeof(*other); ++i) {
        CHECK(!otg_adb_target_is_tcp(other[i]));
        CHECK(!otg_adb_target_prepare(other[i], NULL, NULL));
    }
    CHECK(otg_adb_state_is_device("device\n", 7));
    CHECK(otg_adb_state_is_device("device\r\n", 8));
    CHECK(otg_adb_state_is_device("device", 6));
    CHECK(!otg_adb_state_is_device("offline\n", 8));
    CHECK(!otg_adb_state_is_device("unauthorized\n", 13));
    CHECK(!otg_adb_state_is_device("device extra", 12));
    struct fake f = {.ready = true};
    CHECK(!otg_adb_target_prepare(tcp[0], command, &f));
    CHECK(f.states == 1 && f.connects == 0);
    f = (struct fake){0};
    CHECK(!otg_adb_target_prepare(tcp[0], command, &f));
    CHECK(f.states == 2 && f.connects == 1);
    f = (struct fake){.fail_connect = true};
    CHECK(otg_adb_target_prepare(tcp[0], command, &f) == -1);
    CHECK(f.states == 1 && f.connects == 1);
    f = (struct fake){.stay_offline = true};
    CHECK(otg_adb_target_prepare(tcp[0], command, &f) == -1);
    CHECK(f.states == 2 && f.connects == 1);
    puts("PASS: USB/mDNS unchanged; TCP classification, online check, connect and readiness verification");
    return 0;
}
