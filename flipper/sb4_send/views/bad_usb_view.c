#include "bad_usb_view.h"
#include "../helpers/send_worker.h"
#include <toolbox/path.h>
#include <gui/elements.h>
#include "sb4_send_icons.h"

#define MAX_NAME_LEN 64

struct BadUsb {
    View* view;
    BadUsbButtonCallback callback;
    void* context;
};

typedef struct {
    char file_name[MAX_NAME_LEN];
    char layout[MAX_NAME_LEN];
    BadUsbState state;
    bool pause_wait;
    uint8_t anim_frame;
} BadUsbModel;

/* Part numbers count from 0, as P4SEND's messages and P4S Start Part do. */
static void bad_usb_draw_batch_line(const BadUsbModel* model, FuriString* line) {
    const BadUsbState* st = &model->state;
    if(st->part_count == 0) return;
    BadUsbWorkerState state = st->state;
    if((state == BadUsbStateIdle) || (state == BadUsbStateNotConnected)) {
        if(strlen(model->layout) == 0) {
            furi_string_printf(line, "From part %lu/%lu", st->part_start, st->part_count);
        } else {
            furi_string_printf(
                line, "(%s) from %lu/%lu", model->layout, st->part_start, st->part_count);
        }
    } else if(st->part_only) {
        if(st->part_stage == BadUsbPartStageSend) {
            furi_string_printf(line, "Part %lu only, sending", st->part_cur);
        } else if(st->part_stage == BadUsbPartStageWait) {
            furi_string_printf(line, "Part %lu only, in %lus", st->part_cur, st->wait_left_s);
        } else if(st->part_stage == BadUsbPartStageConfirm) {
            furi_string_printf(line, "Part %lu only: confirming", st->part_cur);
        }
    } else if(st->part_stage == BadUsbPartStageSend) {
        furi_string_printf(line, "Part %lu/%lu sending", st->part_cur, st->part_count);
    } else if(st->part_stage == BadUsbPartStageWait) {
        furi_string_printf(
            line, "Part %lu/%lu in %lus", st->part_cur, st->part_count, st->wait_left_s);
    } else if(st->part_stage == BadUsbPartStageConfirm) {
        furi_string_printf(line, "All %lu sent: confirming", st->part_count);
    }
}

static void bad_usb_draw_remaining(Canvas* canvas, const BadUsbState* st, FuriString* text) {
    if(st->remain_s == UINT32_MAX) return;
    uint32_t s = st->remain_s;
    furi_string_printf(text, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    canvas_draw_str_aligned(canvas, 2, 61, AlignLeft, AlignBottom, furi_string_get_cstr(text));
}

static void bad_usb_draw_callback(Canvas* canvas, void* _model) {
    BadUsbModel* model = _model;

    FuriString* disp_str = furi_string_alloc_set(model->file_name);
    elements_string_fit_width(canvas, disp_str, 128 - 2);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, furi_string_get_cstr(disp_str));

    if(strlen(model->layout) == 0) {
        furi_string_set(disp_str, "(default)");
    } else {
        furi_string_printf(disp_str, "(%s)", model->layout);
    }
    uint32_t e = model->state.elapsed / 1000;
    furi_string_cat_printf(disp_str, "  %02lu:%02lu", e / 60, e % 60);
    bad_usb_draw_batch_line(model, disp_str);
    elements_string_fit_width(canvas, disp_str, 128 - 2);
    canvas_draw_str(
        canvas, 2, 8 + canvas_current_font_height(canvas), furi_string_get_cstr(disp_str));

    furi_string_reset(disp_str);

    canvas_draw_icon(canvas, 22, 24, &I_UsbTree_48x22);

    BadUsbWorkerState state = model->state.state;

    if((state == BadUsbStateIdle) || (state == BadUsbStateDone) ||
       (state == BadUsbStateNotConnected)) {
        elements_button_center(canvas, "Run");
        elements_button_left(canvas, "Config");
        if(model->state.part_count > 0) elements_button_right(canvas, "Part");
    } else if((state == BadUsbStateRunning) || (state == BadUsbStateDelay)) {
        elements_button_center(canvas, "Stop");
        if(!model->pause_wait) {
            elements_button_right(canvas, "Pause");
        }
    } else if(state == BadUsbStatePaused) {
        elements_button_center(canvas, "End");
        elements_button_right(canvas, "Resume");
    } else if(state == BadUsbStateWillRun) {
        elements_button_center(canvas, "Cancel");
    } else if(state == BadUsbStateSendError) {
        elements_button_left(canvas, "Config");
    }

    if(state == BadUsbStateNotConnected) {
        canvas_draw_icon(canvas, 4, 26, &I_Clock_18x18);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 127, 31, AlignRight, AlignBottom, "Connect");
        canvas_draw_str_aligned(canvas, 127, 43, AlignRight, AlignBottom, "to device");
    } else if(state == BadUsbStateWillRun) {
        canvas_draw_icon(canvas, 4, 26, &I_Clock_18x18);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 127, 31, AlignRight, AlignBottom, "Will run");
        canvas_draw_str_aligned(canvas, 127, 43, AlignRight, AlignBottom, "on connect");
    } else if(state == BadUsbStateFileError) {
        canvas_draw_icon(canvas, 4, 26, &I_Error_18x18);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 127, 31, AlignRight, AlignBottom, "File");
        canvas_draw_str_aligned(canvas, 127, 43, AlignRight, AlignBottom, "ERROR");
    } else if(state == BadUsbStateSendError) {
        canvas_draw_icon(canvas, 4, 26, &I_Error_18x18);
        furi_string_set_str(disp_str, model->state.error);
        elements_string_fit_width(canvas, disp_str, canvas_width(canvas));
        canvas_draw_str_aligned(
            canvas, 127, 46, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 127, 33, AlignRight, AlignBottom, "ERROR:");
    } else if(state == BadUsbStateIdle) {
        canvas_draw_icon(canvas, 4, 26, &I_Smile_18x18);
        furi_string_printf(disp_str, "0/%zu", model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 124, 47, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str_aligned(canvas, 112, 37, AlignRight, AlignBottom, "0");
        canvas_draw_icon(canvas, 115, 23, &I_Percent_10x14);
    } else if(state == BadUsbStateRunning) {
        if(model->anim_frame == 0) {
            canvas_draw_icon(canvas, 4, 23, &I_EviSmile1_18x21);
        } else {
            canvas_draw_icon(canvas, 4, 23, &I_EviSmile2_18x21);
        }
        furi_string_printf(disp_str, "%zu/%zu", model->state.line_cur, model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 124, 47, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_set_font(canvas, FontBigNumbers);
        furi_string_printf(
            disp_str, "%zu", ((model->state.line_cur - 1) * 100) / model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 112, 37, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_draw_icon(canvas, 115, 23, &I_Percent_10x14);
        canvas_set_font(canvas, FontSecondary);
        bad_usb_draw_remaining(canvas, &model->state, disp_str);
    } else if(state == BadUsbStateDone) {
        canvas_draw_icon(canvas, 4, 23, &I_EviSmile1_18x21);
        furi_string_printf(disp_str, "%zu/%zu", model->state.line_nb, model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 124, 47, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str_aligned(canvas, 112, 37, AlignRight, AlignBottom, "100");
        canvas_draw_icon(canvas, 115, 23, &I_Percent_10x14);
    } else if(state == BadUsbStateDelay) {
        if(model->anim_frame == 0) {
            canvas_draw_icon(canvas, 4, 23, &I_EviWaiting1_18x21);
        } else {
            canvas_draw_icon(canvas, 4, 23, &I_EviWaiting2_18x21);
        }
        uint32_t delay = model->state.delay_remain / 10;
        if(model->state.remain_s != UINT32_MAX) {
            bad_usb_draw_remaining(canvas, &model->state, disp_str);
        } else if(delay) {
            furi_string_printf(disp_str, "Delay %lus", delay);
            canvas_draw_str_aligned(
                canvas, 4, 61, AlignLeft, AlignBottom, furi_string_get_cstr(disp_str));
        }
        furi_string_printf(disp_str, "%zu/%zu", model->state.line_cur, model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 124, 47, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_set_font(canvas, FontBigNumbers);
        furi_string_printf(
            disp_str, "%zu", ((model->state.line_cur - 1) * 100) / model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 112, 37, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_draw_icon(canvas, 115, 23, &I_Percent_10x14);
    } else if(state == BadUsbStatePaused) {
        if(model->anim_frame == 0) {
            canvas_draw_icon(canvas, 4, 23, &I_EviWaiting1_18x21);
        } else {
            canvas_draw_icon(canvas, 4, 23, &I_EviWaiting2_18x21);
        }
        canvas_draw_str_aligned(canvas, 4, 61, AlignLeft, AlignBottom, "Paused");
        furi_string_printf(disp_str, "%zu/%zu", model->state.line_cur, model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 124, 47, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_set_font(canvas, FontBigNumbers);
        furi_string_printf(
            disp_str, "%zu", ((model->state.line_cur - 1) * 100) / model->state.line_nb);
        canvas_draw_str_aligned(
            canvas, 112, 37, AlignRight, AlignBottom, furi_string_get_cstr(disp_str));
        canvas_draw_icon(canvas, 115, 23, &I_Percent_10x14);
    } else {
        canvas_draw_icon(canvas, 4, 26, &I_Clock_18x18);
    }

    furi_string_free(disp_str);
}

/* An archive, and nothing being sent: Right or a long OK can send one part. */
static bool bad_usb_view_can_send_part(BadUsb* bad_usb) {
    bool can = false;
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        {
            BadUsbWorkerState state = model->state.state;
            can = model->state.part_count > 0 &&
                  ((state == BadUsbStateIdle) || (state == BadUsbStateDone) ||
                   (state == BadUsbStateNotConnected));
        },
        false);
    return can;
}

static bool bad_usb_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    BadUsb* bad_usb = context;
    bool consumed = false;

    // A long press comes instead of a short one, never with it
    if(((event->type == InputTypeShort && event->key == InputKeyRight) ||
        (event->type == InputTypeLong &&
         (event->key == InputKeyRight || event->key == InputKeyOk))) &&
       bad_usb_view_can_send_part(bad_usb)) {
        furi_assert(bad_usb->callback);
        bad_usb->callback(BadUsbCustomEventSendPart, bad_usb->context);
        return true;
    }

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft) {
            consumed = true;
            furi_assert(bad_usb->callback);
            bad_usb->callback(event->key, bad_usb->context);
        } else if(event->key == InputKeyOk) {
            with_view_model(
                bad_usb->view, BadUsbModel * model, { model->pause_wait = false; }, true);
            consumed = true;
            furi_assert(bad_usb->callback);
            bad_usb->callback(event->key, bad_usb->context);
        } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
            consumed = true;
            furi_assert(bad_usb->callback);
            bad_usb->callback(event->key, bad_usb->context);
        } else if(event->key == InputKeyRight) {
            with_view_model(
                bad_usb->view,
                BadUsbModel * model,
                {
                    if((model->state.state == BadUsbStateRunning) ||
                       (model->state.state == BadUsbStateDelay)) {
                        model->pause_wait = true;
                    }
                },
                true);
            consumed = true;
            furi_assert(bad_usb->callback);
            bad_usb->callback(event->key, bad_usb->context);
        }
    }

    return consumed;
}

BadUsb* bad_usb_view_alloc(void) {
    BadUsb* bad_usb = malloc(sizeof(BadUsb));

    bad_usb->view = view_alloc();
    view_allocate_model(bad_usb->view, ViewModelTypeLocking, sizeof(BadUsbModel));
    view_set_context(bad_usb->view, bad_usb);
    view_set_draw_callback(bad_usb->view, bad_usb_draw_callback);
    view_set_input_callback(bad_usb->view, bad_usb_input_callback);

    return bad_usb;
}

void bad_usb_view_free(BadUsb* bad_usb) {
    furi_assert(bad_usb);
    view_free(bad_usb->view);
    free(bad_usb);
}

View* bad_usb_view_get_view(BadUsb* bad_usb) {
    furi_assert(bad_usb);
    return bad_usb->view;
}

void bad_usb_view_set_button_callback(
    BadUsb* bad_usb,
    BadUsbButtonCallback callback,
    void* context) {
    furi_assert(bad_usb);
    furi_assert(callback);
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        {
            UNUSED(model);
            bad_usb->callback = callback;
            bad_usb->context = context;
        },
        true);
}

void bad_usb_view_set_file_name(BadUsb* bad_usb, const char* name) {
    furi_assert(name);
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        { strlcpy(model->file_name, name, MAX_NAME_LEN); },
        true);
}

void bad_usb_view_set_layout(BadUsb* bad_usb, const char* layout) {
    furi_assert(layout);
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        { strlcpy(model->layout, layout, MAX_NAME_LEN); },
        true);
}

void bad_usb_view_set_state(BadUsb* bad_usb, BadUsbState* st) {
    furi_assert(st);
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        {
            memcpy(&(model->state), st, sizeof(BadUsbState));
            model->anim_frame ^= 1;
            if(model->state.state == BadUsbStatePaused) {
                model->pause_wait = false;
            }
        },
        true);
}

bool bad_usb_view_can_config(BadUsb* bad_usb) {
    bool can = bad_usb_view_is_idle_state(bad_usb);
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        {
            if(model->state.state == BadUsbStateSendError) {
                can = true;
            }
        },
        false);
    return can;
}

bool bad_usb_view_is_idle_state(BadUsb* bad_usb) {
    bool is_idle = false;
    with_view_model(
        bad_usb->view,
        BadUsbModel * model,
        {
            if((model->state.state == BadUsbStateIdle) ||
               (model->state.state == BadUsbStateDone) ||
               (model->state.state == BadUsbStateNotConnected)) {
                is_idle = true;
            }
        },
        false);
    return is_idle;
}
