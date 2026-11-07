#pragma once

#include <gui/view.h>
#include "../helpers/send_worker.h"

typedef struct BadUsb BadUsb;
/** An InputKey for a short press, or this. */
#define BadUsbCustomEventSendPart 0x100
typedef void (*BadUsbButtonCallback)(uint32_t event, void* context);

BadUsb* bad_usb_view_alloc(void);

void bad_usb_view_free(BadUsb* bad_usb);

View* bad_usb_view_get_view(BadUsb* bad_usb);

void bad_usb_view_set_button_callback(
    BadUsb* bad_usb,
    BadUsbButtonCallback callback,
    void* context);

void bad_usb_view_set_file_name(BadUsb* bad_usb, const char* name);

void bad_usb_view_set_layout(BadUsb* bad_usb, const char* layout);

void bad_usb_view_set_state(BadUsb* bad_usb, BadUsbState* st);

bool bad_usb_view_is_idle_state(BadUsb* bad_usb);

/** Idle, or stopped by an error that a setting may cure. */
bool bad_usb_view_can_config(BadUsb* bad_usb);
