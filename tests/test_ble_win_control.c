/* GUI control framing/state regression; no Windows/Bluetooth access.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_win_control.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool feed(struct otg_ble_control *c, const char *text)
{
    return otg_ble_control_feed(c, text, strlen(text));
}
int main(void)
{
    struct otg_ble_control c = {0};
    assert(feed(&c, "pa")); assert(c.phase == OTG_BLE_ACTIVE);
    assert(feed(&c, "use\n")); assert(c.phase == OTG_BLE_PAUSING);
    assert(!feed(&c, "resume\tlocalhost\t24800\tandroid\t1080\t1920\n"));
    c.used = 0; c.phase = OTG_BLE_IDLE;
    const char *resume = "resume\t127.0.0.2\t24801\tphone-2\t540\t1170\n";
    for (size_t i = 0; i < strlen(resume); ++i) assert(otg_ble_control_feed(&c, resume + i, 1));
    assert(c.phase == OTG_BLE_ACTIVE && c.port == 24801 && c.width == 540 && c.height == 1170);
    assert(!strcmp(c.host, "127.0.0.2") && !strcmp(c.name, "phone-2"));
    const char *invalid[] = {
        "resume\tx\t0\tphone\t10\t20\n", "resume\tx\t65536\tphone\t10\t20\n",
        "resume\tx\t24800\tphone\t32768\t20\n", "resume\tx\t+12\tphone\t10\t20\n",
        "resume\tx\t12\tbad name\t10\t20\n", "resume\t\t12\tp\t10\t20\n",
        "resume\tx\t12\tp\t10\t20\textra\n", "wat\n", "pause\r\n"
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        memset(&c, 0, sizeof(c)); c.phase = OTG_BLE_IDLE;
        assert(!feed(&c, invalid[i])); assert(c.phase == OTG_BLE_IDLE);
    }
    memset(&c, 0, sizeof(c));
    char long_line[sizeof(c.line) + 1]; memset(long_line, 'x', sizeof(long_line));
    assert(!otg_ble_control_feed(&c, long_line, sizeof(long_line)));
    memset(&c, 0, sizeof(c));
    const char nul[] = {'p','a','u','s','e',0,'\n'};
    assert(!otg_ble_control_feed(&c, nul, sizeof(nul)));
    memset(&c, 0, sizeof(c));
    assert(feed(&c, "pause\nstop\n")); assert(c.stop);
    puts("PASS: Windows BLE GUI control parser");
    return 0;
}
