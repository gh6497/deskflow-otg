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
    libusb_device *device;  /* ref'd; used to detect unplug */
    bool disconnected;      /* set once the device is gone */
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

/*
 * True if the device has been detected as unplugged.
 */
bool aoa_hid_is_disconnected(const struct aoa_hid *a);

/*
 * Re-check whether the device is still present (re-reads the libusb device
 * list).  Returns true if it is still connected; if the device is gone it
 * marks the handle disconnected and returns false.  Call this periodically
 * (e.g. from the event-loop idle callback) so an unplug is noticed even when
 * no HID report happens to be in flight.
 */
bool aoa_hid_poll(struct aoa_hid *a);

#endif /* DESKFLOW_OTG_AOA_HID_H */
