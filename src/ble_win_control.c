/* GUI control parser, independent of WinRT. SPDX-License-Identifier: Apache-2.0 */
#include "ble_win_control.h"
#include <string.h>

static unsigned number(const char *text, unsigned max)
{
    unsigned value = 0;
    if (!*text) return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || value > max / 10) return 0;
        value = value * 10 + (unsigned)(*text - '0');
        if (value > max) return 0;
    }
    return value;
}

static bool command(struct otg_ble_control *c)
{
    if (!strcmp(c->line, "stop")) { c->stop = true; return true; }
    if (!strcmp(c->line, "pause")) {
        if (c->phase == OTG_BLE_ACTIVE) c->phase = OTG_BLE_PAUSING;
        return true;
    }
    char *fields[6], *p = c->line;
    for (unsigned i = 0; i < 6; ++i) {
        fields[i] = p;
        char *tab = strchr(p, '\t');
        if (i == 5) { if (tab) return false; }
        else { if (!tab) return false; *tab = 0; p = tab + 1; }
    }
    if (strcmp(fields[0], "resume") || c->phase != OTG_BLE_IDLE ||
        !*fields[1] || strlen(fields[1]) >= sizeof(c->host) ||
        !*fields[3] || strlen(fields[3]) >= sizeof(c->name)) return false;
    for (const char *n = fields[3]; *n; ++n)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') ||
              (*n >= '0' && *n <= '9') || *n == '_' || *n == '.' || *n == '-')) return false;
    unsigned port = number(fields[2], 65535), width = number(fields[4], 32767),
             height = number(fields[5], 32767);
    if (!port || !width || !height) return false;
    strcpy(c->host, fields[1]); strcpy(c->name, fields[3]);
    c->port = port; c->width = width; c->height = height;
    c->phase = OTG_BLE_ACTIVE;
    return true;
}

bool otg_ble_control_feed(struct otg_ble_control *c, const char *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        unsigned char b = (unsigned char)bytes[i];
        if (b == '\n') {
            c->line[c->used] = 0;
            if (!command(c)) return false;
            c->used = 0;
            if (c->stop) return true;
        } else {
            if ((b < 32 && b != '\t') || b == 127 || c->used + 1 >= sizeof(c->line)) return false;
            c->line[c->used++] = (char)b;
        }
    }
    return true;
}
