#include "../bad_usb_app_i.h"

/* What an entry of the list is. The list depends on the file and the mode, so an
 * entry's position says nothing: config_items maps one to the other. */
typedef enum {
    ConfigItemSendMode,
    ConfigItemTextDelay,
    ConfigItemTextCapsLock,
    ConfigItemKeyboardLayout,
    ConfigItemPetit4Type,
    ConfigItemPetit4Compress,
    ConfigItemPetit4Link,
    ConfigItemPetit4Parity,
    ConfigItemPetit4Interval,
    ConfigItemPetit4StartPart,
    ConfigItemPetit4PartWait,
    ConfigItemCount,
} ConfigItem;

static uint8_t config_items[ConfigItemCount];
static uint8_t config_item_count;

static void bad_usb_scene_config_select_callback(void* context, uint32_t index) {
    BadUsbApp* bad_usb = context;

    if(index < config_item_count) {
        view_dispatcher_send_custom_event(bad_usb->view_dispatcher, config_items[index]);
    }
}

static const char* const send_mode_names[BadUsbModeCount] = {"Type Text", "Petit4Send"};
static const char* const petit4_type_names[Petit4TypeCount] = {"Auto", "TXT", "DAT", "GRP"};
static const char* const petit4_comp_names[Petit4CompCount] = {"Auto", "LZSS", "None"};
static const uint32_t text_delays[] = {30, 50, 70, 100, 150, 200, 300};
static const uint32_t petit4_intervals[] = {24, 30, 40, 50, 70, 100};
static const uint32_t petit4_parities[] = {0, 16, 32, 64};
static const char* const petit4_parity_names[] = {"Off", "1 in 16", "1 in 32", "1 in 64"};
static const uint32_t petit4_links[] = {0, 6, 7};
static const char* const petit4_link_names[] = {"Classic", "NS1", "NS2"};
static const uint32_t petit4_part_waits[] = {5, 10, 20, 30, 45, 60, 90, 120};

#define COUNT_OF_U32(array) (sizeof(array) / sizeof(uint32_t))

static void set_ms_text(VariableItem* item, uint32_t ms) {
    char text[12];
    snprintf(text, sizeof(text), "%lu ms", ms);
    variable_item_set_current_value_text(item, text);
}

static void bad_usb_scene_config_send_mode_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.mode = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, send_mode_names[bad_usb->send_cfg.mode]);
    // The list cannot be built again from inside its own input
    view_dispatcher_send_custom_event(bad_usb->view_dispatcher, ConfigItemSendMode);
}

static void bad_usb_scene_config_text_delay_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.text_delay = text_delays[variable_item_get_current_value_index(item)];
    set_ms_text(item, bad_usb->send_cfg.text_delay);
}

static void bad_usb_scene_config_text_capslock_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.text_capslock = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, bad_usb->send_cfg.text_capslock ? "ON" : "OFF");
}

static void bad_usb_scene_config_petit4_type_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.petit4_type = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, petit4_type_names[bad_usb->send_cfg.petit4_type]);
}

static void bad_usb_scene_config_petit4_comp_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.petit4_comp = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, petit4_comp_names[bad_usb->send_cfg.petit4_comp]);
}

static void bad_usb_scene_config_petit4_parity_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    // Off is not on offer on the NS links
    uint8_t index = variable_item_get_current_value_index(item) +
                    (bad_usb->send_cfg.petit4_link ? 1 : 0);
    bad_usb->send_cfg.petit4_parity = petit4_parities[index];
    variable_item_set_current_value_text(item, petit4_parity_names[index]);
}

static void bad_usb_scene_config_petit4_link_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    bad_usb->send_cfg.petit4_link = petit4_links[index];
    variable_item_set_current_value_text(item, petit4_link_names[index]);
    view_dispatcher_send_custom_event(bad_usb->view_dispatcher, ConfigItemPetit4Link);
}

static void bad_usb_scene_config_petit4_interval_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.petit4_interval =
        petit4_intervals[variable_item_get_current_value_index(item)];
    set_ms_text(item, bad_usb->send_cfg.petit4_interval);
}

static void set_s_text(VariableItem* item, uint32_t seconds) {
    char text[12];
    snprintf(text, sizeof(text), "%lu s", seconds);
    variable_item_set_current_value_text(item, text);
}

static void bad_usb_scene_config_petit4_part_wait_callback(VariableItem* item) {
    BadUsbApp* bad_usb = variable_item_get_context(item);
    bad_usb->send_cfg.petit4_part_wait =
        petit4_part_waits[variable_item_get_current_value_index(item)];
    set_s_text(item, bad_usb->send_cfg.petit4_part_wait);
}

static uint8_t index_of_u32(const uint32_t* values, size_t count, uint32_t value) {
    for(size_t i = 0; i < count; i++) {
        if(values[i] == value) return i;
    }
    return 0;
}

static VariableItem* add_item(
    BadUsbApp* bad_usb,
    ConfigItem id,
    const char* label,
    uint8_t values,
    VariableItemChangeCallback callback) {
    config_items[config_item_count++] = id;
    return variable_item_list_add(bad_usb->var_item_list, label, values, callback, bad_usb);
}

static void draw_send_mode_item(BadUsbApp* bad_usb) {
    BadUsbSendConfig* cfg = &bad_usb->send_cfg;
    VariableItem* item = add_item(
        bad_usb,
        ConfigItemSendMode,
        "Send Mode",
        BadUsbModeCount,
        bad_usb_scene_config_send_mode_callback);
    variable_item_set_current_value_index(item, cfg->mode);
    variable_item_set_current_value_text(item, send_mode_names[cfg->mode]);
}

static void draw_text_items(BadUsbApp* bad_usb) {
    BadUsbSendConfig* cfg = &bad_usb->send_cfg;
    VariableItem* item;

    item = add_item(
        bad_usb,
        ConfigItemTextDelay,
        "Text Key Delay",
        COUNT_OF_U32(text_delays),
        bad_usb_scene_config_text_delay_callback);
    variable_item_set_current_value_index(
        item, index_of_u32(text_delays, COUNT_OF_U32(text_delays), cfg->text_delay));
    set_ms_text(item, cfg->text_delay);

    item = add_item(
        bad_usb,
        ConfigItemTextCapsLock,
        "Text CapsLock",
        2,
        bad_usb_scene_config_text_capslock_callback);
    variable_item_set_current_value_index(item, cfg->text_capslock);
    variable_item_set_current_value_text(item, cfg->text_capslock ? "ON" : "OFF");

    add_item(bad_usb, ConfigItemKeyboardLayout, "Keyboard Layout", 0, NULL);
}

static void draw_petit4_link_item(BadUsbApp* bad_usb) {
    VariableItem* item = add_item(
        bad_usb, ConfigItemPetit4Link, "P4S Link", 3, bad_usb_scene_config_petit4_link_callback);
    uint8_t index = index_of_u32(petit4_links, 3, bad_usb->send_cfg.petit4_link);
    variable_item_set_current_value_index(item, index);
    variable_item_set_current_value_text(item, petit4_link_names[index]);
}

static void draw_petit4_file_items(BadUsbApp* bad_usb) {
    BadUsbSendConfig* cfg = &bad_usb->send_cfg;
    VariableItem* item;

    item = add_item(
        bad_usb,
        ConfigItemPetit4Type,
        "P4S File Type",
        Petit4TypeCount,
        bad_usb_scene_config_petit4_type_callback);
    variable_item_set_current_value_index(item, cfg->petit4_type);
    variable_item_set_current_value_text(item, petit4_type_names[cfg->petit4_type]);

    item = add_item(
        bad_usb,
        ConfigItemPetit4Compress,
        "P4S Compress",
        Petit4CompCount,
        bad_usb_scene_config_petit4_comp_callback);
    variable_item_set_current_value_index(item, cfg->petit4_comp);
    variable_item_set_current_value_text(item, petit4_comp_names[cfg->petit4_comp]);

    draw_petit4_link_item(bad_usb);

    // Only P4SENDX reads it; a batch has its own, chosen in p4pack
    bool multi = cfg->petit4_link;
    uint8_t first = multi ? 1 : 0;
    item = add_item(
        bad_usb,
        ConfigItemPetit4Parity,
        "P4S Parity",
        COUNT_OF_U32(petit4_parities) - first,
        bad_usb_scene_config_petit4_parity_callback);
    uint32_t parity = multi && !cfg->petit4_parity ? PETIT4_MULTI_PARITY : cfg->petit4_parity;
    uint8_t parity_index = index_of_u32(petit4_parities, COUNT_OF_U32(petit4_parities), parity);
    variable_item_set_current_value_index(item, parity_index - first);
    variable_item_set_current_value_text(item, petit4_parity_names[parity_index]);
}

static void draw_petit4_interval_item(BadUsbApp* bad_usb) {
    BadUsbSendConfig* cfg = &bad_usb->send_cfg;
    VariableItem* item = add_item(
        bad_usb,
        ConfigItemPetit4Interval,
        "P4S Interval",
        COUNT_OF_U32(petit4_intervals),
        bad_usb_scene_config_petit4_interval_callback);
    variable_item_set_current_value_index(
        item, index_of_u32(petit4_intervals, COUNT_OF_U32(petit4_intervals), cfg->petit4_interval));
    set_ms_text(item, cfg->petit4_interval);
}

static void draw_petit4_batch_items(BadUsbApp* bad_usb) {
    BadUsbSendConfig* cfg = &bad_usb->send_cfg;
    VariableItem* item;

    // Far more parts than a value list can hold: OK opens a number entry
    item = add_item(bad_usb, ConfigItemPetit4StartPart, "P4S Start Part", 1, NULL);
    char start_part[12];
    snprintf(start_part, sizeof(start_part), "%lu", cfg->petit4_start_part);
    variable_item_set_current_value_text(item, start_part);

    item = add_item(
        bad_usb,
        ConfigItemPetit4PartWait,
        "P4S Part Wait",
        COUNT_OF_U32(petit4_part_waits),
        bad_usb_scene_config_petit4_part_wait_callback);
    variable_item_set_current_value_index(
        item,
        index_of_u32(petit4_part_waits, COUNT_OF_U32(petit4_part_waits), cfg->petit4_part_wait));
    set_s_text(item, cfg->petit4_part_wait);
}

static void draw_menu(BadUsbApp* bad_usb) {
    VariableItemList* var_item_list = bad_usb->var_item_list;

    variable_item_list_reset(var_item_list);
    variable_item_list_set_enter_callback(
        var_item_list, bad_usb_scene_config_select_callback, bad_usb);
    config_item_count = 0;

    if(bad_usb->file_is_archive) {
        draw_petit4_link_item(bad_usb);
        draw_petit4_interval_item(bad_usb);
        draw_petit4_batch_items(bad_usb);
    } else if(bad_usb->send_cfg.mode == BadUsbModePetit4) {
        draw_send_mode_item(bad_usb);
        draw_petit4_file_items(bad_usb);
        draw_petit4_interval_item(bad_usb);
    } else {
        draw_send_mode_item(bad_usb);
        draw_text_items(bad_usb);
    }

    variable_item_list_set_selected_item(
        var_item_list, scene_manager_get_scene_state(bad_usb->scene_manager, BadUsbSceneConfig));
}

void bad_usb_scene_config_on_enter(void* context) {
    BadUsbApp* bad_usb = context;

    bad_usb_app_check_file(bad_usb);
    draw_menu(bad_usb);

    view_dispatcher_switch_to_view(bad_usb->view_dispatcher, BadUsbAppViewConfig);
}

bool bad_usb_scene_config_on_event(void* context, SceneManagerEvent event) {
    BadUsbApp* bad_usb = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(
            bad_usb->scene_manager,
            BadUsbSceneConfig,
            variable_item_list_get_selected_item_index(bad_usb->var_item_list));
        consumed = true;

        switch(event.event) {
        case ConfigItemSendMode:
        case ConfigItemPetit4Link:
            draw_menu(bad_usb);
            break;
        case ConfigItemKeyboardLayout:
            scene_manager_next_scene(bad_usb->scene_manager, BadUsbSceneConfigLayout);
            break;
        case ConfigItemPetit4StartPart:
            scene_manager_next_scene(bad_usb->scene_manager, BadUsbSceneConfigStartPart);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void bad_usb_scene_config_on_exit(void* context) {
    BadUsbApp* bad_usb = context;
    VariableItemList* var_item_list = bad_usb->var_item_list;

    variable_item_list_reset(var_item_list);
}
