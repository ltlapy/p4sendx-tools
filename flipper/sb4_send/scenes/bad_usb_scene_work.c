#include "../helpers/send_worker.h"
#include "../bad_usb_app_i.h"
#include "../views/bad_usb_view.h"
#include <furi_hal.h>
#include "toolbox/path.h"

void bad_usb_scene_work_button_callback(uint32_t event, void* context) {
    furi_assert(context);
    BadUsbApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

/* Slow, Detect, then the sync keys, one way or the other round. */
static int16_t next_petit4_sync(int16_t sync, bool up) {
    int16_t index = sync == BAD_USB_PETIT4_SLOW ? 0 : sync == BAD_USB_PETIT4_DETECT ? 1 : sync + 2;
    int16_t count = PETIT4_SYNC_KEYS + 2;
    index = (index + (up ? 1 : count - 1)) % count;
    return index == 0 ? BAD_USB_PETIT4_SLOW : index == 1 ? BAD_USB_PETIT4_DETECT : index - 2;
}

static void show_petit4_sync(BadUsbApp* app) {
    if(bad_usb_app_mode(app) != BadUsbModePetit4) return;
    if(app->send_cfg.petit4_link) {
        bad_usb_view_set_layout(
            app->bad_usb_view, app->send_cfg.petit4_link == 6 ? "NS1" : "NS2");
        return;
    }
    int16_t sync = bad_usb_sender_get_petit4_sync(app->bad_usb_sender);
    char label[20];
    if(sync == BAD_USB_PETIT4_SLOW) {
        strlcpy(label, "P4S Slow", sizeof(label));
    } else if(sync == BAD_USB_PETIT4_DETECT) {
        strlcpy(label, "P4S Detect", sizeof(label));
    } else {
        snprintf(label, sizeof(label), "P4S Key %d", sync);
    }
    bad_usb_view_set_layout(app->bad_usb_view, label);
}

/* The console may sound at the same time. Every note on both sides is of the C
 * major pentatonic scale at A4 = 440 Hz, so that any overlap is consonant. Stock
 * sequences have other pitches. Rising means done. */
static const NotificationSequence sound_done = {
    &message_note_c5,
    &message_delay_100,
    &message_note_e5,
    &message_delay_100,
    &message_note_g5,
    &message_delay_100,
    &message_note_c6,
    &message_delay_250,
    &message_sound_off,
    NULL,
};

/* A falling figure, repeated: the user is needed. */
static const NotificationSequence sound_needed = {
    &message_note_a5,
    &message_delay_100,
    &message_note_e5,
    &message_delay_250,
    &message_sound_off,
    &message_delay_100,
    &message_note_a5,
    &message_delay_100,
    &message_note_e5,
    &message_delay_250,
    &message_sound_off,
    &message_delay_100,
    &message_note_a5,
    &message_delay_100,
    &message_note_e5,
    &message_delay_250,
    &message_sound_off,
    NULL,
};

/* Sounds for what needs the user, played from this thread: a message to the
 * notification service, on the record the app holds open. */
static void play_sounds(BadUsbApp* app) {
    const BadUsbState* st = bad_usb_sender_get_state(app->bad_usb_sender);
    BadUsbWorkerState state = st->state;
    BadUsbPartStage stage = st->part_stage;
    BadUsbWorkerState was = app->sound_state;
    bool stage_new = stage != app->sound_stage;
    app->sound_state = state;
    app->sound_stage = stage;

    if(state != was) {
        if(state == BadUsbStateDone) {
            notification_message(app->notifications, &sound_done);
        } else if(state == BadUsbStateSendError || state == BadUsbStateFileError) {
            notification_message(app->notifications, &sound_needed);
        } else if(
            state == BadUsbStateNotConnected &&
            (was == BadUsbStateRunning || was == BadUsbStateDelay || was == BadUsbStatePaused)) {
            // Cut off in the middle, as by the console going to sleep
            notification_message(app->notifications, &sound_needed);
        }
    }
    if(stage_new && stage == BadUsbPartStageConfirm) {
        notification_message(app->notifications, &sound_done);
    }
}

bool bad_usb_scene_work_on_event(void* context, SceneManagerEvent event) {
    BadUsbApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyLeft) {
            if(bad_usb_view_can_config(app->bad_usb_view)) {
                bad_usb_sender_close(app->bad_usb_sender);
                app->bad_usb_sender = NULL;

                scene_manager_set_scene_state(app->scene_manager, BadUsbSceneConfig, 0);
                scene_manager_next_scene(app->scene_manager, BadUsbSceneConfig);
            }
            consumed = true;
        } else if(event.event == BadUsbCustomEventSendPart) {
            // The sender stays open, and USB with it, so a sync key is kept
            if(bad_usb_view_is_idle_state(app->bad_usb_view) &&
               bad_usb_sender_get_state(app->bad_usb_sender)->part_count > 0) {
                scene_manager_next_scene(app->scene_manager, BadUsbSceneSendPart);
            }
            consumed = true;
        } else if(event.event == InputKeyOk) {
            bad_usb_sender_start_stop(app->bad_usb_sender);
            consumed = true;
        } else if(event.event == InputKeyUp || event.event == InputKeyDown) {
            // Sync keys are for Classic: this link has none to choose
            if(bad_usb_app_mode(app) == BadUsbModePetit4 && !app->send_cfg.petit4_link &&
               bad_usb_view_is_idle_state(app->bad_usb_view)) {
                bad_usb_sender_set_petit4_sync(
                    app->bad_usb_sender,
                    next_petit4_sync(
                        bad_usb_sender_get_petit4_sync(app->bad_usb_sender),
                        event.event == InputKeyUp));
                show_petit4_sync(app);
            }
            consumed = true;
        } else if(event.event == InputKeyRight) {
            if(!bad_usb_view_is_idle_state(app->bad_usb_view)) {
                bad_usb_sender_pause_resume(app->bad_usb_sender);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        bad_usb_view_set_state(app->bad_usb_view, bad_usb_sender_get_state(app->bad_usb_sender));
        show_petit4_sync(app);
        play_sounds(app);
    }
    return consumed;
}

void bad_usb_scene_work_on_enter(void* context) {
    BadUsbApp* app = context;

    if(app->bad_usb_sender) {
        // Back from Send Part, with the sender still open
        bad_usb_view_set_state(
            app->bad_usb_view, bad_usb_sender_get_state(app->bad_usb_sender));
        view_dispatcher_switch_to_view(app->view_dispatcher, BadUsbAppViewWork);
        return;
    }

    app->sound_state = BadUsbStateInit;
    app->sound_stage = BadUsbPartStageNone;
    bad_usb_app_check_file(app);
    // An archive goes as Petit4Send whatever the saved mode is, which stays as it was
    BadUsbSendConfig send_cfg = app->send_cfg;
    send_cfg.mode = bad_usb_app_mode(app);
    app->bad_usb_sender = bad_usb_sender_open(app->file_path, &send_cfg);
    bad_usb_sender_set_keyboard_layout(app->bad_usb_sender, app->keyboard_layout);

    FuriString* file_name;
    file_name = furi_string_alloc();
    path_extract_filename(app->file_path, file_name, true);
    bad_usb_view_set_file_name(app->bad_usb_view, furi_string_get_cstr(file_name));
    furi_string_free(file_name);

    FuriString* layout;
    layout = furi_string_alloc();
    path_extract_filename(app->keyboard_layout, layout, true);
    if(send_cfg.mode == BadUsbModeText) {
        furi_string_cat_printf(layout, " typing");
    }
    bad_usb_view_set_layout(app->bad_usb_view, furi_string_get_cstr(layout));
    show_petit4_sync(app);
    furi_string_free(layout);

    bad_usb_view_set_state(app->bad_usb_view, bad_usb_sender_get_state(app->bad_usb_sender));

    bad_usb_view_set_button_callback(app->bad_usb_view, bad_usb_scene_work_button_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BadUsbAppViewWork);
}

void bad_usb_scene_work_on_exit(void* context) {
    UNUSED(context);
}
