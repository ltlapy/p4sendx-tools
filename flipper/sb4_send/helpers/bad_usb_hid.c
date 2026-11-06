#include "bad_usb_hid.h"
#include "petit4send_usb.h"
#include "petit4send_multi_usb.h"

#define TAG "SB4Send HID"

static FuriHalUsbHidConfig hid_usb_config = {
    .vid = HID_VID_DEFAULT,
    .pid = HID_PID_DEFAULT,
};

static void* hid_usb_init(void) {
    FuriHalUsbInterface* usb_if_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_hid, &hid_usb_config));
    return usb_if_prev;
}

static void hid_usb_deinit(void* inst) {
    FuriHalUsbInterface* usb_if_prev = inst;
    furi_check(furi_hal_usb_set_config(usb_if_prev, NULL));
}

static void hid_usb_set_state_callback(void* inst, HidStateCallback cb, void* context) {
    UNUSED(inst);
    furi_hal_hid_set_state_callback(cb, context);
}

static bool hid_usb_is_connected(void* inst) {
    UNUSED(inst);
    return furi_hal_hid_is_connected();
}

static bool hid_usb_kb_press(void* inst, uint16_t button) {
    UNUSED(inst);
    return furi_hal_hid_kb_press(button);
}

static bool hid_usb_kb_release(void* inst, uint16_t button) {
    UNUSED(inst);
    return furi_hal_hid_kb_release(button);
}

static bool hid_usb_release_all(void* inst) {
    UNUSED(inst);
    bool state = furi_hal_hid_kb_release_all();
    state &= furi_hal_hid_consumer_key_release_all();
    state &= furi_hal_hid_mouse_release(0);
    return state;
}

static const BadUsbHidApi hid_api_usb = {
    .init = hid_usb_init,
    .deinit = hid_usb_deinit,
    .set_state_callback = hid_usb_set_state_callback,
    .is_connected = hid_usb_is_connected,

    .kb_press = hid_usb_kb_press,
    .kb_release = hid_usb_kb_release,
    .release_all = hid_usb_release_all,
};

const BadUsbHidApi* bad_usb_hid_get_usb(void) {
    return &hid_api_usb;
}

static void* hid_petit4_init(void) {
    petit4_usb_start(0, 0);
    return NULL;
}

static void hid_petit4_deinit(void* inst) {
    UNUSED(inst);
    petit4_usb_stop();
}

static void hid_petit4_set_state_callback(void* inst, HidStateCallback cb, void* context) {
    UNUSED(inst);
    petit4_usb_set_state_callback(cb, context);
}

static bool hid_petit4_is_connected(void* inst) {
    UNUSED(inst);
    return petit4_usb_is_connected();
}

static bool hid_petit4_release_all(void* inst) {
    UNUSED(inst);
    return petit4_usb_release_all();
}

static const BadUsbHidApi hid_api_petit4 = {
    .init = hid_petit4_init,
    .deinit = hid_petit4_deinit,
    .set_state_callback = hid_petit4_set_state_callback,
    .is_connected = hid_petit4_is_connected,
    .release_all = hid_petit4_release_all,
};

const BadUsbHidApi* bad_usb_hid_get_petit4(void) {
    return &hid_api_petit4;
}

static void* hid_multi_init(void) {
    petit4_multi_usb_start();
    return NULL;
}

static void hid_multi_deinit(void* inst) {
    UNUSED(inst);
    petit4_multi_usb_stop();
}

static void hid_multi_set_state_callback(void* inst, HidStateCallback cb, void* context) {
    UNUSED(inst);
    petit4_multi_usb_set_state_callback(cb, context);
}

static bool hid_multi_is_connected(void* inst) {
    UNUSED(inst);
    return petit4_multi_usb_is_connected();
}

static bool hid_multi_release_all(void* inst) {
    UNUSED(inst);
    return petit4_multi_usb_release_all();
}

static const BadUsbHidApi hid_api_multi = {
    .init = hid_multi_init,
    .deinit = hid_multi_deinit,
    .set_state_callback = hid_multi_set_state_callback,
    .is_connected = hid_multi_is_connected,
    .release_all = hid_multi_release_all,
};

const BadUsbHidApi* bad_usb_hid_get_petit4_multi(void) {
    return &hid_api_multi;
}
