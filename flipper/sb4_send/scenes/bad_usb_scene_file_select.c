#include "../bad_usb_app_i.h"
#include <storage/storage.h>

static bool bad_usb_file_select(BadUsbApp* bad_usb) {
    furi_assert(bad_usb);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BAD_USB_APP_BASE_FOLDER);
    furi_record_close(RECORD_STORAGE);

    // Momentum's DialogsFileBrowserOptions ends with one more field, which its
    // dialog_file_browser_set_basic_options writes: built with the official SDK, the
    // spare bytes take it.
    struct {
        DialogsFileBrowserOptions options;
        uint8_t spare[8];
    } browser = {0};
    DialogsFileBrowserOptions* browser_options = &browser.options;
    // Any file of the card: what can be sent does not depend on where it is
    dialog_file_browser_set_basic_options(browser_options, "*", &I_badusb_10px);
    browser_options->base_path = STORAGE_EXT_PATH_PREFIX;
    browser_options->hide_ext = false;
    browser_options->skip_assets = true;

    // Input events and views are managed by file_browser
    bool res = dialog_file_browser_show(
        bad_usb->dialogs, bad_usb->file_path, bad_usb->file_path, browser_options);

    return res;
}

void bad_usb_scene_file_select_on_enter(void* context) {
    BadUsbApp* bad_usb = context;

    if(bad_usb->bad_usb_sender) {
        bad_usb_sender_close(bad_usb->bad_usb_sender);
        bad_usb->bad_usb_sender = NULL;
    }

    if(bad_usb_file_select(bad_usb)) {
        scene_manager_next_scene(bad_usb->scene_manager, BadUsbSceneWork);
    } else {
        view_dispatcher_stop(bad_usb->view_dispatcher);
    }
}

bool bad_usb_scene_file_select_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void bad_usb_scene_file_select_on_exit(void* context) {
    UNUSED(context);
}
