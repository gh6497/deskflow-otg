/* USB enumeration and notifications: no AOA requests, interface claims or ADB. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_USB_SCAN_H
#define OTG_USB_SCAN_H

int otg_usb_scan(void);
/* Returns 3 when hotplug is unavailable. stop_check watches GUI stdin/EOF. */
int otg_usb_watch(int (*stop_check)(void));

#endif
