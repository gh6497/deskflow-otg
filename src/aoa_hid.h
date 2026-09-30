/*
 * aoa_hid.h - USB AOA (Android Open Accessory) HID layer.
 *
 * This is a faithful, dependency-free port of scrcpy's USB/AOA/HID code:
 *   - app/src/usb/usb.c            (device enumeration + open)
 *   - app/src/usb/aoa_hid.c        (HID register / send control transfers)
 *   - app/src/hid/hid_keyboard.c   (keyboard report descriptor + encoding)
 *   - app/src/hid/hid_mouse.c      (mouse report descriptor + encoding)
 *
 * It implements the AOAv2 HID protocol over libusb so that the host appears
 * to the Android device as a physical USB HID keyboard + mouse.  No adb and no
 * USB debugging are required (mirroring `scrcpy --otg`).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_AOA_HID_H
#define DESKFLOW_OTG_AOA_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libusb-1.0/libusb.h>

/* AOA HID accessory IDs (must be unique per registered HID device). */
#define AOA_HID_ID_KEYBOARD 1
#define AOA_HID_ID_MOUSE    2

#define AOA_KEYBOARD_REPORT_SIZE 8
#define AOA_MOUSE_REPORT_SIZE    5

struct aoa_hid {
    libusb_context *ctx;
    libusb_device_handle *handle;
};

/*
 * Initialize libusb.  Returns true on success.
 */
bool aoa_hid_init(struct aoa_hid *a);

/* Release all libusb resources. */
void aoa_hid_destroy(struct aoa_hid *a);

/*
 * Open the Android USB device.
 *
 * If `serial` is non-NULL, the device whose serial number matches is opened;
 * otherwise the single eligible device (one that reports a serial number) is
 * used.  Returns true on success.
 */
bool aoa_hid_open(struct aoa_hid *a, const char *serial);

/* Close the device handle (does not tear down the libusb context). */
void aoa_hid_close(struct aoa_hid *a);

/*
 * Register an AOA HID device and install its report descriptor.
 *
 * `id` is the accessory-assigned HID ID, `report_desc`/`report_desc_size`
 * describe the HID report.  Mirrors scrcpy's sc_aoa_setup_hid().
 */
bool aoa_hid_register(struct aoa_hid *a, uint16_t id,
                      const uint8_t *report_desc, uint16_t report_desc_size);

/*
 * Send one HID input report (mirrors scrcpy's sc_aoa_send_hid_event()).
 */
bool aoa_hid_send(struct aoa_hid *a, uint16_t id,
                  const uint8_t *data, uint16_t size);

#endif /* DESKFLOW_OTG_AOA_HID_H */
