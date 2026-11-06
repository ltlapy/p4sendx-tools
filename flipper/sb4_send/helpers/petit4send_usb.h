#pragma once

/* USB keyboard and mouse for PETIT4SEND. The firmware's own HID mode cannot be
 * used for this: its keyboard stops at usage 101, its mouse has three buttons,
 * and it sends a report for each key rather than one for all six. */

#include <furi.h>
#include <furi_hal.h>

#include "petit4send.h"

/* Momentum's furi_hal_usb_hid.h has these; the official SDK keeps them private. */
#ifndef HID_VID_DEFAULT
#define HID_VID_DEFAULT 0x046D
#endif
#ifndef HID_PID_DEFAULT
#define HID_PID_DEFAULT 0xC529
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Petit4UsbStateCallback)(bool connected, void* context);

/** Takes over USB. vid and pid 0 mean the defaults of the firmware's HID mode. */
void petit4_usb_start(uint16_t vid, uint16_t pid);

/** Gives USB back to the mode it had before. */
void petit4_usb_stop(void);

bool petit4_usb_is_connected(void);

/** A clock in ms from the USB host's frames, which keeps time with its VSYNC. */
uint32_t petit4_usb_ms(void);

void petit4_usb_set_state_callback(Petit4UsbStateCallback callback, void* context);

/** Sends the mouse report, then the keyboard report with the clock in it. */
bool petit4_usb_send(const Petit4Packet* packet);

/** Nothing held any more. */
bool petit4_usb_release_all(void);

#ifdef __cplusplus
}
#endif
