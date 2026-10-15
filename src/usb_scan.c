/* USB descriptor enumeration, following scrcpy's usb/usb.c. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_scan.h"
#include <libusb-1.0/libusb.h>
#include <stdio.h>

static void json_string(const unsigned char *text, int length)
{
    putchar('"');
    for (int i = 0; i < length; ++i) {
        unsigned char c = text[i];
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c < 0x20 || c >= 0x7f) {
            printf("\\u%04x", (unsigned)c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

int otg_usb_scan(void)
{
    libusb_context *ctx = NULL;
    int error = libusb_init(&ctx);
    if (error < 0) {
        fprintf(stderr, "ERROR: libusb_init: %s\n", libusb_strerror(error));
        return 1;
    }
    libusb_device **devices = NULL;
    ssize_t count = libusb_get_device_list(ctx, &devices);
    if (count < 0) {
        fprintf(stderr, "ERROR: libusb_get_device_list: %s\n", libusb_strerror((int)count));
        libusb_exit(ctx);
        return 1;
    }
    printf("{\"version\":1,\"devices\":[");
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *device = devices[i];
        struct libusb_device_descriptor desc;
        unsigned char serial[256];
        int length = 0;
        unsigned vid = 0, pid = 0;
        if (libusb_get_device_descriptor(device, &desc) == 0) {
            vid = desc.idVendor;
            pid = desc.idProduct;
            libusb_device_handle *handle = NULL;
            if (desc.iSerialNumber && libusb_open(device, &handle) == 0) {
                length = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber,
                                                            serial, sizeof(serial));
                libusb_close(handle);
            }
        }
        /* Keep unreadable devices in the snapshot. Permission/descriptor errors
         * must not look like an unplug and re-arm a previously attempted device.
         * Bus/address identify this attachment, not the remembered phone. */
        printf("%s{\"id\":\"%u:%u\",\"vid\":%u,\"pid\":%u,\"serial\":",
               i ? "," : "", (unsigned)libusb_get_bus_number(device),
               (unsigned)libusb_get_device_address(device), vid, pid);
        json_string(serial, length > 0 ? length : 0);
        putchar('}');
    }
    puts("]}");
    libusb_free_device_list(devices, 1);
    libusb_exit(ctx);
    return ferror(stdout) ? 1 : 0;
}

static int LIBUSB_CALL on_hotplug(libusb_context *ctx, libusb_device *device,
                                  libusb_hotplug_event event, void *userdata)
{
    (void)ctx;
    (void)userdata;
    /* No descriptor reads or USB I/O in the callback. It may run on libusb's
     * event thread. The GUI coalesces notifications and scans separately. */
    printf("{\"event\":\"%s\",\"id\":\"%u:%u\"}\n",
           event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT ? "usb-removed" : "usb-added",
           (unsigned)libusb_get_bus_number(device),
           (unsigned)libusb_get_device_address(device));
    fflush(stdout);
    return 0;
}

int otg_usb_watch(int (*stop_check)(void))
{
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        return 3;
    }
    libusb_context *ctx = NULL;
    int error = libusb_init(&ctx);
    if (error < 0) {
        fprintf(stderr, "ERROR: libusb_init: %s\n", libusb_strerror(error));
        return 1;
    }
    libusb_hotplug_callback_handle callback;
    error = libusb_hotplug_register_callback(ctx,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        0, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, on_hotplug, NULL, &callback);
    if (error < 0) {
        fprintf(stderr, "ERROR: hotplug registration: %s\n", libusb_strerror(error));
        libusb_exit(ctx);
        return 3;
    }
    /* Arm notifications before the GUI's initial scan to avoid a startup gap.
     * Do not ENUMERATE: the GUI performs one authoritative snapshot instead. */
    puts("{\"event\":\"watch-ready\",\"version\":1}");
    fflush(stdout);
    while (!stop_check() && !ferror(stdout)) {
        /* This waits for OS/libusb events, it does not enumerate USB devices.
         * The timeout only bounds detection of parent exit/stdin cancellation. */
        struct timeval timeout = {0, 250000};
        error = libusb_handle_events_timeout_completed(ctx, &timeout, NULL);
        if (error < 0 && error != LIBUSB_ERROR_INTERRUPTED) {
            fprintf(stderr, "ERROR: hotplug events: %s\n", libusb_strerror(error));
            break;
        }
    }
    libusb_hotplug_deregister_callback(ctx, callback);
    libusb_exit(ctx);
    return error < 0 && error != LIBUSB_ERROR_INTERRUPTED ? 1 : 0;
}
