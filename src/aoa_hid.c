/*
 * aoa_hid.c - see aoa_hid.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aoa_hid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AOAv2 HID control requests (source.android.com/devices/accessories/aoa2). */
#define AOA_REQUEST_REGISTER_HID        54
#define AOA_REQUEST_UNREGISTER_HID      55
#define AOA_REQUEST_SET_HID_REPORT_DESC 56
#define AOA_REQUEST_SEND_HID_EVENT      57

#define AOA_TIMEOUT_MS 1000

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)
#define LOG_DBG(...)  fprintf(stderr, "DEBUG: " __VA_ARGS__)

bool aoa_hid_init(struct aoa_hid *a)
{
    memset(a, 0, sizeof(*a));
    int r = libusb_init(&a->ctx);
    if (r < 0) {
        LOG_ERR("libusb_init: %s\n", libusb_strerror(r));
        return false;
    }
    return true;
}

void aoa_hid_destroy(struct aoa_hid *a)
{
    if (a->handle) {
        libusb_close(a->handle);
        a->handle = NULL;
    }
    if (a->ctx) {
        libusb_exit(a->ctx);
        a->ctx = NULL;
    }
}

/*
 * Read an ASCII USB string descriptor (mirrors scrcpy's read_string()).
 */
static char *read_string(libusb_device_handle *handle, uint8_t index)
{
    unsigned char buf[128];
    int r = libusb_get_string_descriptor_ascii(handle, index, buf,
                                               (int)sizeof(buf));
    if (r < 0) {
        return NULL;
    }
    char *s = malloc((size_t)r + 1);
    if (!s) {
        return NULL;
    }
    memcpy(s, buf, (size_t)r);
    s[r] = '\0';
    return s;
}

bool aoa_hid_open(struct aoa_hid *a, const char *serial)
{
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(a->ctx, &list);
    if (count < 0) {
        LOG_ERR("libusb_get_device_list: %s\n", libusb_strerror((int)count));
        return false;
    }

    libusb_device *chosen = NULL;
    char *chosen_serial = NULL;
    bool multiple = false;

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *dev = list[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) < 0 || !desc.iSerialNumber) {
            continue; /* skip devices without a serial number */
        }

        libusb_device_handle *h = NULL;
        if (libusb_open(dev, &h) < 0) {
            continue; /* expected: some devices are not openable */
        }

        char *s = read_string(h, desc.iSerialNumber);
        libusb_close(h);

        if (!s) {
            continue;
        }

        if (serial) {
            if (strcmp(serial, s) == 0) {
                chosen = dev;
                chosen_serial = s;
                break;
            }
            free(s);
        } else {
            if (!chosen) {
                chosen = dev;
                chosen_serial = s;
            } else {
                multiple = true;
                free(s);
            }
        }
    }

    bool ok = false;
    if (multiple && !serial) {
        LOG_ERR("multiple USB devices found, select one with -s/--serial\n");
    } else if (chosen) {
        int r = libusb_open(chosen, &a->handle);
        if (r < 0) {
            LOG_ERR("open device '%s': %s\n", chosen_serial,
                    libusb_strerror(r));
        } else {
            LOG_INFO("opened device %04x:%04x serial=%s\n",
                     libusb_get_bus_number(chosen),
                     libusb_get_device_address(chosen), chosen_serial);
            ok = true;
        }
    } else {
        LOG_ERR("could not find any Android USB device%s%s\n",
                serial ? " with serial " : "", serial ? serial : "");
    }

    free(chosen_serial);
    libusb_free_device_list(list, 1);
    return ok;
}

void aoa_hid_close(struct aoa_hid *a)
{
    if (a->handle) {
        libusb_close(a->handle);
        a->handle = NULL;
    }
}

/*
 * Mirrors scrcpy's sc_aoa_register_hid().
 */
static bool register_hid(struct aoa_hid *a, uint16_t id, uint16_t report_size)
{
    uint8_t bmRequestType = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR;
    int r = libusb_control_transfer(a->handle, bmRequestType,
                                    AOA_REQUEST_REGISTER_HID,
                                    id,        /* wValue: accessory HID id */
                                    report_size, /* wIndex: report desc size */
                                    NULL, 0, AOA_TIMEOUT_MS);
    if (r < 0) {
        LOG_ERR("REGISTER_HID(%u): %s\n", id, libusb_strerror(r));
        return false;
    }
    return true;
}

/*
 * Mirrors scrcpy's sc_aoa_set_hid_report_desc().
 */
static bool set_hid_report_desc(struct aoa_hid *a, uint16_t id,
                                const uint8_t *desc, uint16_t desc_size)
{
    uint8_t bmRequestType = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR;
    /* libusb handles multi-packet splitting internally. */
    int r = libusb_control_transfer(
        a->handle, bmRequestType, AOA_REQUEST_SET_HID_REPORT_DESC,
        id, 0, (unsigned char *)desc, desc_size, AOA_TIMEOUT_MS);
    if (r < 0) {
        LOG_ERR("SET_HID_REPORT_DESC(%u): %s\n", id, libusb_strerror(r));
        return false;
    }
    return true;
}

bool aoa_hid_register(struct aoa_hid *a, uint16_t id,
                      const uint8_t *report_desc, uint16_t report_desc_size)
{
    if (!register_hid(a, id, report_desc_size)) {
        return false;
    }
    if (!set_hid_report_desc(a, id, report_desc, report_desc_size)) {
        /* best-effort cleanup, matching scrcpy */
        libusb_control_transfer(a->handle,
                                LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                AOA_REQUEST_UNREGISTER_HID, id, 0, NULL, 0,
                                AOA_TIMEOUT_MS);
        return false;
    }
    return true;
}

bool aoa_hid_send(struct aoa_hid *a, uint16_t id,
                  const uint8_t *data, uint16_t size)
{
    uint8_t bmRequestType = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR;
    int r = libusb_control_transfer(a->handle, bmRequestType,
                                    AOA_REQUEST_SEND_HID_EVENT,
                                    id, 0, (unsigned char *)data, size,
                                    AOA_TIMEOUT_MS);
    if (r < 0) {
        LOG_ERR("SEND_HID_EVENT(%u): %s\n", id, libusb_strerror(r));
        return false;
    }
    return true;
}
