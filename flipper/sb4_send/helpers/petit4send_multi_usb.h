#pragma once

/* 6 or 7 boot keyboards for the NS links, changed from SB4 Probe's probe_usb.c. The
 * Switch reads 42 keys at once this way, but no report wider than a boot keyboard's. */

#include <furi.h>
#include <furi_hal.h>

#include "petit4send.h"
#include "petit4send_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How many, before petit4_multi_usb_start. */
void petit4_multi_usb_set_keyboards(uint8_t count);

/** Takes over USB. */
void petit4_multi_usb_start(void);

/** Gives USB back to the mode it had before. */
void petit4_multi_usb_stop(void);

bool petit4_multi_usb_is_connected(void);

void petit4_multi_usb_set_state_callback(Petit4UsbStateCallback callback, void* context);

/** Sends what changed, the keyboard with the clock last. */
bool petit4_multi_usb_send(const Petit4MultiPacket* packet);

/** Nothing held any more. */
bool petit4_multi_usb_release_all(void);

#ifdef __cplusplus
}
#endif
