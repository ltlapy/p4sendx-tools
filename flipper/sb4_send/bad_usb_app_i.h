#pragma once

#include "bad_usb_app.h"
#include "scenes/bad_usb_scene.h"
#include "helpers/send_worker.h"
#include "helpers/bad_usb_hid.h"

#include <gui/gui.h>
#include "sb4_send_icons.h"
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <dialogs/dialogs.h>
#include <notification/notification_messages.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/number_input.h>
#include "views/bad_usb_view.h"

#define BAD_USB_APP_BASE_FOLDER        EXT_PATH("badusb")
#define BAD_USB_APP_PATH_LAYOUT_FOLDER BAD_USB_APP_BASE_FOLDER "/assets/layouts"
#define BAD_USB_APP_LAYOUT_EXTENSION   ".kl"

struct BadUsbApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    NotificationApp* notifications;
    DialogsApp* dialogs;
    VariableItemList* var_item_list;
    NumberInput* number_input;

    FuriString* file_path;
    FuriString* keyboard_layout;
    BadUsb* bad_usb_view;
    BadUsbSender* bad_usb_sender;

    BadUsbSendConfig send_cfg;
    bool send_mode_unknown; // The settings hold a mode this build lacks: chosen by the file
    bool file_is_archive;
    uint32_t send_part; // Send Part's choice, which is not saved
    BadUsbWorkerState sound_state;
    BadUsbPartStage sound_stage;
};

typedef enum {
    BadUsbAppViewWork,
    BadUsbAppViewConfig,
    BadUsbAppViewNumberInput,
} BadUsbAppView;

/** Looks at the chosen file: whether it is an archive, and the mode if none is known. */
void bad_usb_app_check_file(BadUsbApp* app);

/** How the chosen file goes, which is not always the saved mode. */
BadUsbMode bad_usb_app_mode(const BadUsbApp* app);
