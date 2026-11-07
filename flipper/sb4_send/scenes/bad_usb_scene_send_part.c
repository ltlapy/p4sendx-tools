#include "../bad_usb_app_i.h"

enum SendPartResult {
    SendPartResultOk,
};

static void bad_usb_scene_send_part_callback(void* context, int32_t number) {
    BadUsbApp* bad_usb = context;
    bad_usb->send_part = number;
    view_dispatcher_send_custom_event(bad_usb->view_dispatcher, SendPartResultOk);
}

/* Opened from the Run screen of an archive, while nothing is sent. Back returns
 * without sending. */
void bad_usb_scene_send_part_on_enter(void* context) {
    BadUsbApp* bad_usb = context;
    uint32_t count = bad_usb_sender_get_state(bad_usb->bad_usb_sender)->part_count;
    uint32_t part = bad_usb->send_cfg.petit4_start_part;
    if(part >= count) part = count - 1;
    number_input_set_header_text(bad_usb->number_input, "Send Part");
    number_input_set_result_callback(
        bad_usb->number_input, bad_usb_scene_send_part_callback, bad_usb, part, 0, count - 1);
    view_dispatcher_switch_to_view(bad_usb->view_dispatcher, BadUsbAppViewNumberInput);
}

bool bad_usb_scene_send_part_on_event(void* context, SceneManagerEvent event) {
    BadUsbApp* bad_usb = context;
    if(event.type == SceneManagerEventTypeCustom) {
        bad_usb_sender_send_part(bad_usb->bad_usb_sender, bad_usb->send_part);
        scene_manager_previous_scene(bad_usb->scene_manager);
        return true;
    }
    return false;
}

void bad_usb_scene_send_part_on_exit(void* context) {
    UNUSED(context);
}
