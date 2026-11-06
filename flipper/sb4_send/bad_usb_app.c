#include "bad_usb_app_i.h"
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <lib/toolbox/path.h>
#include <flipper_format/flipper_format.h>

#define BAD_USB_SETTINGS_PATH           BAD_USB_APP_BASE_FOLDER "/.sb4send.settings"
#define BAD_USB_SETTINGS_FILE_TYPE      "Flipper SB4 Send Settings File"
#define BAD_USB_SETTINGS_VERSION        1
#define BAD_USB_SETTINGS_DEFAULT_LAYOUT BAD_USB_APP_PATH_LAYOUT_FOLDER "/en-US.kl"

static bool bad_usb_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    BadUsbApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool bad_usb_app_back_event_callback(void* context) {
    furi_assert(context);
    BadUsbApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void bad_usb_app_tick_event_callback(void* context) {
    furi_assert(context);
    BadUsbApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static void bad_usb_default_send_settings(BadUsbSendConfig* send_cfg) {
    send_cfg->mode = BadUsbModeText;
    send_cfg->text_delay = 70;
    send_cfg->text_capslock = true;
    send_cfg->petit4_type = Petit4TypeAuto;
    send_cfg->petit4_comp = Petit4CompAuto;
    send_cfg->petit4_interval = 24;
    send_cfg->petit4_start_part = 0;
    send_cfg->petit4_part_wait = 10;
    send_cfg->petit4_parity = 0;
    send_cfg->petit4_link = 0;
}

static void bad_usb_load_send_settings(BadUsbApp* app, FlipperFormat* fff) {
    BadUsbSendConfig* send_cfg = &app->send_cfg;
    uint32_t value = 0;
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "send_mode", &value, 1)) {
        if(value < BadUsbModeCount) {
            send_cfg->mode = value;
        } else {
            // A mode this app no longer has
            app->send_mode_unknown = true;
        }
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "text_delay", &value, 1) && value > 0 && value <= 1000) {
        send_cfg->text_delay = value;
    }
    flipper_format_rewind(fff);
    bool flag = false;
    if(flipper_format_read_bool(fff, "text_capslock", &flag, 1)) {
        send_cfg->text_capslock = flag;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_type", &value, 1) && value < Petit4TypeCount) {
        send_cfg->petit4_type = value;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_comp", &value, 1) && value < Petit4CompCount) {
        send_cfg->petit4_comp = value;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_interval", &value, 1) && value >= 17 &&
       value <= 1000) {
        send_cfg->petit4_interval = value;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_start_part", &value, 1) &&
       value < PETIT4_BATCH_MAX_PARTS) {
        send_cfg->petit4_start_part = value;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_part_wait", &value, 1) && value >= 5 &&
       value <= 120) {
        send_cfg->petit4_part_wait = value;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_parity", &value, 1) &&
       (value == 0 || value == 16 || value == 32 || value == 64)) {
        send_cfg->petit4_parity = value;
    }
    flipper_format_rewind(fff);
    if(flipper_format_read_uint32(fff, "petit4_link", &value, 1)) {
        if(value == 1) value = 7; // as it was saved when there was one such link
        if(value == 0 || value == 6 || value == 7) send_cfg->petit4_link = value;
    }
}

static void bad_usb_load_settings(BadUsbApp* app) {
    bad_usb_default_send_settings(&app->send_cfg);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* fff = flipper_format_file_alloc(storage);
    bool loaded = false;

    FuriString* temp_str = furi_string_alloc();
    uint32_t temp_uint = 0;

    if(flipper_format_file_open_existing(fff, BAD_USB_SETTINGS_PATH)) {
        do {
            if(!flipper_format_read_header(fff, temp_str, &temp_uint)) break;
            if((strcmp(furi_string_get_cstr(temp_str), BAD_USB_SETTINGS_FILE_TYPE) != 0) ||
               (temp_uint != BAD_USB_SETTINGS_VERSION))
                break;

            if(flipper_format_read_string(fff, "layout", temp_str)) {
                furi_string_set(app->keyboard_layout, temp_str);
                FileInfo layout_file_info;
                FS_Error file_check_err = storage_common_stat(
                    storage, furi_string_get_cstr(app->keyboard_layout), &layout_file_info);
                if((file_check_err != FSE_OK) || (layout_file_info.size != 256)) {
                    furi_string_set(app->keyboard_layout, BAD_USB_SETTINGS_DEFAULT_LAYOUT);
                }
            } else {
                furi_string_set(app->keyboard_layout, BAD_USB_SETTINGS_DEFAULT_LAYOUT);
                flipper_format_rewind(fff);
            }

            bad_usb_load_send_settings(app, fff);

            loaded = true;
        } while(0);
    }

    furi_string_free(temp_str);

    flipper_format_free(fff);
    furi_record_close(RECORD_STORAGE);

    if(!loaded) {
        furi_string_set(app->keyboard_layout, BAD_USB_SETTINGS_DEFAULT_LAYOUT);
    }
}

static void bad_usb_save_settings(BadUsbApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* fff = flipper_format_file_alloc(storage);
    uint32_t temp_uint = 0;

    if(flipper_format_file_open_always(fff, BAD_USB_SETTINGS_PATH)) {
        do {
            if(!flipper_format_write_header_cstr(
                   fff, BAD_USB_SETTINGS_FILE_TYPE, BAD_USB_SETTINGS_VERSION))
                break;
            if(!flipper_format_write_string(fff, "layout", app->keyboard_layout)) break;
            const BadUsbSendConfig* send_cfg = &app->send_cfg;
            temp_uint = send_cfg->mode;
            if(!app->send_mode_unknown &&
               !flipper_format_write_uint32(fff, "send_mode", &temp_uint, 1))
                break;
            if(!flipper_format_write_uint32(fff, "text_delay", &send_cfg->text_delay, 1)) break;
            if(!flipper_format_write_bool(fff, "text_capslock", &send_cfg->text_capslock, 1)) break;
            temp_uint = send_cfg->petit4_type;
            if(!flipper_format_write_uint32(fff, "petit4_type", &temp_uint, 1)) break;
            temp_uint = send_cfg->petit4_comp;
            if(!flipper_format_write_uint32(fff, "petit4_comp", &temp_uint, 1)) break;
            if(!flipper_format_write_uint32(fff, "petit4_interval", &send_cfg->petit4_interval, 1))
                break;
            if(!flipper_format_write_uint32(
                   fff, "petit4_start_part", &send_cfg->petit4_start_part, 1))
                break;
            if(!flipper_format_write_uint32(fff, "petit4_part_wait", &send_cfg->petit4_part_wait, 1))
                break;
            if(!flipper_format_write_uint32(fff, "petit4_parity", &send_cfg->petit4_parity, 1))
                break;
            if(!flipper_format_write_uint32(fff, "petit4_link", &send_cfg->petit4_link, 1)) break;
        } while(0);
    }

    flipper_format_free(fff);
    furi_record_close(RECORD_STORAGE);
}

static bool bad_usb_file_is_typed(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot && (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".prg") == 0 ||
                   strcasecmp(dot, ".bas") == 0);
}

void bad_usb_app_check_file(BadUsbApp* app) {
    uint8_t head[4];
    size_t head_len = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, furi_string_get_cstr(app->file_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        head_len = storage_file_read(file, head, sizeof(head));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    app->file_is_archive = petit4_batch_is_p4a(head, head_len);
    if(app->send_mode_unknown && !app->file_is_archive) {
        app->send_cfg.mode = bad_usb_file_is_typed(furi_string_get_cstr(app->file_path)) ?
                                 BadUsbModeText :
                                 BadUsbModePetit4;
        app->send_mode_unknown = false;
    }
}

BadUsbMode bad_usb_app_mode(const BadUsbApp* app) {
    return app->file_is_archive ? BadUsbModePetit4 : app->send_cfg.mode;
}

BadUsbApp* bad_usb_app_alloc(char* arg) {
    BadUsbApp* app = malloc(sizeof(BadUsbApp));

    app->bad_usb_sender = NULL;

    app->file_path = furi_string_alloc();
    app->keyboard_layout = furi_string_alloc();
    if(arg && strlen(arg)) {
        furi_string_set(app->file_path, arg);
    }

    bad_usb_load_settings(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&bad_usb_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, bad_usb_app_tick_event_callback, 250);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, bad_usb_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, bad_usb_app_back_event_callback);

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        BadUsbAppViewConfig,
        variable_item_list_get_view(app->var_item_list));

    app->bad_usb_view = bad_usb_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadUsbAppViewWork, bad_usb_view_get_view(app->bad_usb_view));

    app->number_input = number_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadUsbAppViewNumberInput, number_input_get_view(app->number_input));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    if(!furi_string_empty(app->file_path)) {
        scene_manager_next_scene(app->scene_manager, BadUsbSceneWork);
    } else {
        furi_string_set(app->file_path, BAD_USB_APP_BASE_FOLDER);
        scene_manager_next_scene(app->scene_manager, BadUsbSceneFileSelect);
    }

    return app;
}

void bad_usb_app_free(BadUsbApp* app) {
    furi_assert(app);

    if(app->bad_usb_sender) {
        bad_usb_sender_close(app->bad_usb_sender);
        app->bad_usb_sender = NULL;
    }

    // Views
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewWork);
    bad_usb_view_free(app->bad_usb_view);

    // Config menu
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewConfig);
    variable_item_list_free(app->var_item_list);

    // Number Input
    view_dispatcher_remove_view(app->view_dispatcher, BadUsbAppViewNumberInput);
    number_input_free(app->number_input);

    // View dispatcher
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    // Close records
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);

    bad_usb_save_settings(app);

    furi_string_free(app->file_path);
    furi_string_free(app->keyboard_layout);

    free(app);
}

int32_t bad_usb_app(void* p) {
    BadUsbApp* bad_usb_app = bad_usb_app_alloc((char*)p);

    view_dispatcher_run(bad_usb_app->view_dispatcher);

    bad_usb_app_free(bad_usb_app);
    return 0;
}
