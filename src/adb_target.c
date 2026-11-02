/* Shared wireless ADB startup policy; reference: Android adb CLI.
 * SPDX-License-Identifier: Apache-2.0 */
#include "adb_target.h"

#include <stdio.h>
#include <string.h>

static bool ascii_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

bool otg_adb_target_is_tcp(const char *serial)
{
    const char *port, *host_end, *host;
    unsigned number = 0;
    bool ipv6;
    if (!serial || !*serial || strlen(serial) > 255) return false;
    ipv6 = *serial == '[';
    host = serial + (ipv6 ? 1 : 0);
    host_end = ipv6 ? strchr(host, ']') : strchr(host, ':');
    if (!host_end || host == host_end) return false;
    if (ipv6) {
        if (host_end[1] != ':') return false;
        port = host_end + 2;
    } else {
        if (!ascii_alnum(*host)) return false;
        port = host_end + 1;
    }
    for (const char *p = host; p < host_end; ++p) {
        if (!ascii_alnum(*p) && *p != '.' && *p != '-' && *p != '_' &&
            !(ipv6 && (*p == ':' || *p == '%'))) return false;
    }
    if (!*port) return false;
    for (const char *p = port; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        number = number * 10 + (unsigned)(*p - '0');
        if (number > 65535) return false;
    }
    return number != 0;
}

bool otg_adb_state_is_device(const char *output, size_t size)
{
    while (size && (output[size - 1] == '\n' || output[size - 1] == '\r')) --size;
    return size == 6 && !memcmp(output, "device", 6);
}

int otg_adb_target_prepare(const char *serial, otg_adb_target_command command, void *ud)
{
    if (!otg_adb_target_is_tcp(serial)) return 0;
    if (command(ud, serial, false) == 0) return 0;
    fprintf(stderr, "INFO: connecting wireless ADB target %s\n", serial);
    if (command(ud, serial, true) != 0 || command(ud, serial, false) != 0) {
        fprintf(stderr, "ERROR: wireless ADB target %s is not ready; check the phone's "
                "wireless debugging connection port, network and pairing (adb pair)\n", serial);
        return -1;
    }
    return 0;
}
