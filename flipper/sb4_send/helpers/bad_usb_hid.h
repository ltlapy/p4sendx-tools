#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>
#include <furi_hal.h>

typedef struct {
    void* (*init)(void);
    void (*deinit)(void* inst);
    void (*set_state_callback)(void* inst, HidStateCallback cb, void* context);
    bool (*is_connected)(void* inst);

    bool (*kb_press)(void* inst, uint16_t button);
    bool (*kb_release)(void* inst, uint16_t button);
    bool (*release_all)(void* inst);
} BadUsbHidApi;

/** The firmware's USB keyboard, for typing. */
const BadUsbHidApi* bad_usb_hid_get_usb(void);

/** The USB keyboard and mouse PETIT4SEND needs. */
const BadUsbHidApi* bad_usb_hid_get_petit4(void);

/** The 7 boot keyboards of the NS2 link. */
const BadUsbHidApi* bad_usb_hid_get_petit4_multi(void);

#ifdef __cplusplus
}
#endif
