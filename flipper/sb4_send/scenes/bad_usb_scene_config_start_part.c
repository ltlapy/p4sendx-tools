#include "../bad_usb_app_i.h"

enum NumberInputResult {
    NumberInputResultOk,
};

static void bad_usb_scene_config_start_part_callback(void* context, int32_t number) {
    BadUsbApp* bad_usb = context;
    bad_usb->send_cfg.petit4_start_part = number;
    view_dispatcher_send_custom_event(bad_usb->view_dispatcher, NumberInputResultOk);
}

void bad_usb_scene_config_start_part_on_enter(void* context) {
    BadUsbApp* bad_usb = context;
    number_input_set_header_text(bad_usb->number_input, "Start Part");
    number_input_set_result_callback(
        bad_usb->number_input,
        bad_usb_scene_config_start_part_callback,
        bad_usb,
        bad_usb->send_cfg.petit4_start_part,
        0,
        PETIT4_BATCH_MAX_PARTS - 1);
    view_dispatcher_switch_to_view(bad_usb->view_dispatcher, BadUsbAppViewNumberInput);
}

bool bad_usb_scene_config_start_part_on_event(void* context, SceneManagerEvent event) {
    BadUsbApp* bad_usb = context;
    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_previous_scene(bad_usb->scene_manager);
        return true;
    }
    return false;
}

void bad_usb_scene_config_start_part_on_exit(void* context) {
    UNUSED(context);
}
