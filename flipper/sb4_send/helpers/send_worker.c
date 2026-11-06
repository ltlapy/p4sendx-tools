#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include "send_worker.h"
#include "send_worker_i.h"
#include "petit4send_usb.h"
#include "petit4send_multi_usb.h"
#include <dolphin/dolphin.h>

#define TAG "BadUsb"

#define WORKER_TAG TAG "Worker"

#define BADUSB_ASCII_TO_KEY(sender, x) \
    (((uint8_t)x < 128) ? (sender->layout[(uint8_t)x]) : HID_KEYBOARD_NONE)

typedef enum {
    WorkerEvtStartStop = (1 << 0),
    WorkerEvtPauseResume = (1 << 1),
    WorkerEvtEnd = (1 << 2),
    WorkerEvtConnect = (1 << 3),
    WorkerEvtDisconnect = (1 << 4),
} WorkerEvtFlags;

static void bad_usb_hid_state_callback(bool state, void* context) {
    furi_assert(context);
    BadUsbSender* bad_usb = context;

    // The clock behind a sync key starts again with the connection
    bad_usb->petit4_sync = BAD_USB_PETIT4_SLOW;

    if(state == true) {
        furi_thread_flags_set(furi_thread_get_id(bad_usb->thread), WorkerEvtConnect);
    } else {
        furi_thread_flags_set(furi_thread_get_id(bad_usb->thread), WorkerEvtDisconnect);
    }
}

/* Lines are only for the Run screen's count. */
static bool text_preload(BadUsbSender* bad_usb, File* file) {
    uint8_t buf[TEXT_COUNT_BUFFER_LEN];
    uint8_t ret = 0;
    uint32_t line_len = 0;

    // In keys, for the remaining time: what the indentation sends is not what the file holds
    Petit4Indent indent;
    petit4_indent_init(&indent);
    bad_usb->text_size = 0;

    do {
        ret = storage_file_read(file, buf, sizeof(buf));
        for(uint16_t i = 0; i < ret; i++) {
            bool typed = petit4_indent_byte(&indent, buf[i]);
            while(petit4_indent_key(&indent) != Petit4IndentNone)
                bad_usb->text_size++;
            if(typed && (buf[i] == '\n' || buf[i] == '\t' || (buf[i] >= ' ' && buf[i] < 0x7F))) {
                bad_usb->text_size++;
            }
            if(buf[i] == '\n' && line_len > 0) {
                bad_usb->st.line_nb++;
                line_len = 0;
            } else {
                line_len++;
            }
        }
        if(storage_file_eof(file)) {
            if(line_len > 0) {
                bad_usb->st.line_nb++;
                break;
            }
        }
    } while(ret > 0);
    petit4_indent_end(&indent);
    while(petit4_indent_key(&indent) != Petit4IndentNone)
        bad_usb->text_size++;

    if(bad_usb->send_cfg.mode == BadUsbModePetit4 && bad_usb->send_cfg.petit4_link)
        petit4_multi_usb_set_keyboards(bad_usb->send_cfg.petit4_link);
    bad_usb->hid_inst = bad_usb->hid->init();
    bad_usb->hid_started = true;
    bad_usb->hid->set_state_callback(bad_usb->hid_inst, bad_usb_hid_state_callback, bad_usb);

    storage_file_seek(file, 0, true);

    return true;
}

static size_t petit4_file_read(void* context, uint8_t* buf, size_t len) {
    return storage_file_read(context, buf, len);
}

static bool petit4_file_seek(void* context, uint32_t offset) {
    return storage_file_seek(context, offset, true);
}

static bool petit4_batch_preload(
    BadUsbSender* bad_usb,
    File* file,
    const uint8_t* head,
    size_t head_len) {
    Petit4Batch batch;
    uint32_t start_part = bad_usb->send_cfg.petit4_start_part;
    const char* error =
        petit4_batch_parse(&batch, head, head_len, storage_file_size(file), start_part);
    if(error) {
        if(batch.part_count > 0) {
            snprintf(
                bad_usb->st.error,
                sizeof(bad_usb->st.error),
                "Start Part over %lu",
                batch.part_count - 1);
        } else {
            strlcpy(bad_usb->st.error, error, sizeof(bad_usb->st.error));
        }
        return false;
    }
    petit4_run_init(
        &bad_usb->petit4_run, &batch, start_part, bad_usb->send_cfg.petit4_part_wait * 1000);
    petit4_run_set_multi(&bad_usb->petit4_run, bad_usb->send_cfg.petit4_link);
    if(!petit4_run_open(&bad_usb->petit4_run, bad_usb->petit4)) {
        strlcpy(bad_usb->st.error, petit4_error(bad_usb->petit4), sizeof(bad_usb->st.error));
        return false;
    }
    bad_usb->petit4_batch = true;
    bad_usb->st.part_start = start_part;
    bad_usb->st.part_count = batch.part_count;
    return true;
}

static bool petit4_preload(BadUsbSender* bad_usb, File* file) {
    bad_usb->petit4 = petit4_alloc(petit4_file_read, petit4_file_seek, file);
    petit4_set_multi(bad_usb->petit4, bad_usb->send_cfg.petit4_link);
    uint8_t head[PETIT4_BATCH_HEAD_SIZE];
    size_t head_len = storage_file_seek(file, 0, true) ? storage_file_read(file, head, sizeof(head)) : 0;
    if(petit4_batch_is_p4a(head, head_len)) {
        if(!petit4_batch_preload(bad_usb, file, head, head_len)) return false;
    } else if(!petit4_prepare(
                  bad_usb->petit4,
                  furi_string_get_cstr(bad_usb->file_path),
                  bad_usb->send_cfg.petit4_type,
                  bad_usb->send_cfg.petit4_comp) ||
              !petit4_set_parity(bad_usb->petit4, bad_usb->send_cfg.petit4_parity)) {
        strlcpy(bad_usb->st.error, petit4_error(bad_usb->petit4), sizeof(bad_usb->st.error));
        return false;
    }
    // A single file's parity has just been set: this link has to have some
    petit4_set_multi(bad_usb->petit4, bad_usb->send_cfg.petit4_link);
    bad_usb->st.line_nb = petit4_total(bad_usb->petit4);

    if(bad_usb->send_cfg.mode == BadUsbModePetit4 && bad_usb->send_cfg.petit4_link)
        petit4_multi_usb_set_keyboards(bad_usb->send_cfg.petit4_link);
    bad_usb->hid_inst = bad_usb->hid->init();
    bad_usb->hid_started = true;
    bad_usb->hid->set_state_callback(bad_usb->hid_inst, bad_usb_hid_state_callback, bad_usb);
    return true;
}

static bool send_preload(BadUsbSender* bad_usb, File* file) {
    if(bad_usb->send_cfg.mode == BadUsbModePetit4) {
        return petit4_preload(bad_usb, file);
    }
    return text_preload(bad_usb, file);
}

static void petit4_stream_begin(BadUsbSender* bad_usb) {
    petit4_restart(bad_usb->petit4);
    bad_usb->petit4_started = false;
    bad_usb->petit4_last_step = UINT32_MAX;
    bad_usb->send_marked = false;
    // A late packet in fast mode can be a lost one
    furi_thread_set_current_priority(
        bad_usb->petit4_sync == BAD_USB_PETIT4_SLOW ? FuriThreadPriorityNormal :
                                                      FuriThreadPriorityHigh);
}

static void send_restart(BadUsbSender* bad_usb) {
    bad_usb->text_stage = 0;
    bad_usb->text_done = 0;
    petit4_indent_init(&bad_usb->text_indent);
    bad_usb->text_held = false;
    bad_usb->text_ended = false;
    bad_usb->send_marked = false;
    bad_usb->st.remain_s = PETIT4_REMAIN_NONE;
    bad_usb->st.part_stage = BadUsbPartStageNone;
    bad_usb->remain_tick = furi_get_tick() - 1000;
    if(bad_usb->send_cfg.mode == BadUsbModePetit4) {
        if(bad_usb->petit4_batch) {
            petit4_run_restart(&bad_usb->petit4_run, furi_get_tick());
            if(bad_usb->petit4_sync != BAD_USB_PETIT4_DETECT) {
                // The first step of the run starts the part
                bad_usb->st.line_cur = 1;
                furi_thread_set_current_priority(FuriThreadPriorityNormal);
                return;
            }
        }
        petit4_stream_begin(bad_usb);
    }
}

static void petit4_show_position(BadUsbSender* bad_usb) {
    bad_usb->st.line_cur = petit4_position(bad_usb->petit4) + 1;
    if(bad_usb->st.line_cur > bad_usb->st.line_nb) bad_usb->st.line_cur = bad_usb->st.line_nb;
}

/* The counter as a key, for P4SEND's DETECT SYNC KEY, until stopped. */
static int32_t petit4_execute_detect(BadUsbSender* bad_usb) {
    uint32_t now = petit4_usb_ms();
    uint32_t step = now / 8;
    if(step != bad_usb->petit4_last_step) {
        bad_usb->petit4_last_step = step;
        Petit4Packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.keys[0] = petit4_detect_key(now);
        petit4_usb_send(&packet);
        bad_usb->st.line_cur = 1;
    }
    return 8 - (now % 8);
}

/* A packet when its time has come; the next is read ahead, as the card can
 * take a while and the time to send cannot wait for it. */
static int32_t petit4_execute_fast(BadUsbSender* bad_usb, uint8_t sync) {
    uint32_t now = petit4_usb_ms();
    if(!bad_usb->petit4_started) {
        bad_usb->petit4_started = true;
        bad_usb->petit4_has_pending =
            petit4_next_packet(bad_usb->petit4, &bad_usb->petit4_pending);
        bad_usb->petit4_next_ms = petit4_fast_next(now, sync);
    } else if((int32_t)(now - bad_usb->petit4_next_ms) >= 0) {
        if(!bad_usb->petit4_has_pending) return SEND_STATE_END;
        petit4_usb_send(&bad_usb->petit4_pending);
        bad_usb->petit4_next_ms = petit4_fast_after_send(now, sync);
        bad_usb->petit4_has_pending =
            petit4_next_packet(bad_usb->petit4, &bad_usb->petit4_pending);
        petit4_show_position(bad_usb);
    }
    int32_t wait = (int32_t)(bad_usb->petit4_next_ms - petit4_usb_ms());
    return wait > 0 ? wait : 0;
}

/* One packet, and the time until the next one. */
static int32_t petit4_execute_stream(BadUsbSender* bad_usb) {
    int16_t sync = bad_usb->petit4_sync;
    if(sync >= 0) return petit4_execute_fast(bad_usb, sync);

    uint32_t start = furi_get_tick();
    if(bad_usb->send_cfg.petit4_link) {
        Petit4MultiPacket packet;
        if(!petit4_next_multi(bad_usb->petit4, &packet)) return SEND_STATE_END;
        petit4_multi_usb_send(&packet);
    } else {
        Petit4Packet packet;
        if(!petit4_next_packet(bad_usb->petit4, &packet)) return SEND_STATE_END;
        petit4_usb_send(&packet);
    }
    petit4_show_position(bad_usb);

    uint32_t spent = furi_get_tick() - start;
    uint32_t interval = bad_usb->send_cfg.petit4_interval;
    return spent < interval ? (int32_t)(interval - spent) : 0;
}

/* Enter for a save prompt, on whichever USB this link has. */
static void petit4_send_tap(BadUsbSender* bad_usb, const Petit4Packet* packet) {
    if(bad_usb->send_cfg.petit4_link) {
        Petit4MultiPacket multi;
        petit4_multi_from_packet(packet, &multi);
        petit4_multi_usb_send(&multi);
    } else {
        petit4_usb_send(packet);
    }
}

/* The order of things is in petit4_run_step, which is tested on a host. */
static int32_t petit4_execute_batch(BadUsbSender* bad_usb) {
    Petit4Run* run = &bad_usb->petit4_run;
    if(run->stage == Petit4RunStageSend) {
        int32_t delay = petit4_execute_stream(bad_usb);
        if(delay != SEND_STATE_END) return delay;
        bad_usb->hid->release_all(bad_usb->hid_inst);
        furi_thread_set_current_priority(FuriThreadPriorityNormal);
        petit4_run_part_sent(run, furi_get_tick());
        return 0;
    }

    Petit4Packet packet;
    int32_t wait = 0;
    switch(petit4_run_step(run, bad_usb->petit4, furi_get_tick(), &packet, &wait)) {
    case Petit4RunEventTapPress:
        petit4_send_tap(bad_usb, &packet);
        petit4_tap_done(&run->tap, Petit4TapPress, furi_get_tick());
        return 0;
    case Petit4RunEventTapRelease:
        petit4_send_tap(bad_usb, &packet);
        petit4_tap_done(&run->tap, Petit4TapRelease, furi_get_tick());
        return 0;
    case Petit4RunEventStartStream:
        petit4_stream_begin(bad_usb);
        bad_usb->st.line_nb = petit4_total(bad_usb->petit4);
        bad_usb->st.line_cur = 1;
        return 0;
    case Petit4RunEventError:
        strlcpy(bad_usb->st.error, petit4_error(bad_usb->petit4), sizeof(bad_usb->st.error));
        return SEND_STATE_ERROR;
    default:
        return wait;
    }
}

static int32_t petit4_execute_next(BadUsbSender* bad_usb) {
    if(bad_usb->petit4_sync == BAD_USB_PETIT4_DETECT) return petit4_execute_detect(bad_usb);
    if(bad_usb->petit4_batch) return petit4_execute_batch(bad_usb);
    if(!bad_usb->send_marked) {
        bad_usb->send_marked = true;
        bad_usb->send_mark = bad_usb->st.elapsed;
    }
    return petit4_execute_stream(bad_usb);
}

static void petit4_update_remaining(BadUsbSender* bad_usb, uint32_t now) {
    BadUsbState* st = &bad_usb->st;
    int16_t sync = bad_usb->petit4_sync;
    if(sync == BAD_USB_PETIT4_DETECT) {
        st->remain_s = PETIT4_REMAIN_NONE;
        st->part_stage = BadUsbPartStageNone;
        return;
    }
    if(!bad_usb->petit4_batch) {
        uint32_t rate = bad_usb->send_cfg.petit4_link ?
                            PETIT4_MULTI_RATE_PER_MS * bad_usb->send_cfg.petit4_link / PETIT4_MULTI_KEYBOARDS /
                                bad_usb->send_cfg.petit4_interval :
                        sync >= 0 ? PETIT4_FAST_RATE :
                                    petit4_slow_rate(bad_usb->send_cfg.petit4_interval);
        uint32_t send_ms = bad_usb->send_marked ? st->elapsed - bad_usb->send_mark : 0;
        size_t total = petit4_total(bad_usb->petit4);
        size_t position = petit4_position(bad_usb->petit4);
        if(position > total) position = total;
        st->remain_s = petit4_remaining_s(total - position, position, send_ms, rate);
        return;
    }
    const Petit4Run* run = &bad_usb->petit4_run;
    uint32_t packet_us = sync >= 0 ? PETIT4_FAST_PACKET_US :
                                     bad_usb->send_cfg.petit4_interval * 1000;
    st->remain_s = petit4_run_remaining_s(run, bad_usb->petit4, now, packet_us);
    st->part_cur = run->part;
    st->wait_left_s = petit4_run_wait_left_s(run, now);
    st->part_stage = run->stage == Petit4RunStageSend ? BadUsbPartStageSend :
                     run->stage == Petit4RunStageWait ? BadUsbPartStageWait :
                                                        BadUsbPartStageConfirm;
}

/* For the Run screen, which looks four times a second. */
static void send_update_remaining(BadUsbSender* bad_usb) {
    uint32_t now = furi_get_tick();
    if(now - bad_usb->remain_tick < 250) return;
    bad_usb->remain_tick = now;
    if(bad_usb->send_cfg.mode == BadUsbModePetit4) {
        petit4_update_remaining(bad_usb, now);
    } else {
        uint32_t left =
            bad_usb->text_size > bad_usb->text_done ? bad_usb->text_size - bad_usb->text_done : 0;
        bad_usb->st.remain_s = (uint64_t)left * bad_usb->send_cfg.text_delay / 1000;
    }
}

/* Enter must not stay down through a pause, and a pause uses up no wait. */
static void send_on_pause(BadUsbSender* bad_usb) {
    bad_usb->pause_tick = furi_get_tick();
    if(bad_usb->send_cfg.mode == BadUsbModePetit4 && bad_usb->petit4_batch &&
       bad_usb->petit4_run.tap.down && bad_usb->petit4_run.stage != Petit4RunStageSend) {
        bad_usb->hid->release_all(bad_usb->hid_inst);
        petit4_tap_done(&bad_usb->petit4_run.tap, Petit4TapRelease, furi_get_tick());
    }
}

static void send_on_resume(BadUsbSender* bad_usb) {
    if(bad_usb->send_cfg.mode == BadUsbModePetit4 && bad_usb->petit4_batch) {
        petit4_run_shift(&bad_usb->petit4_run, furi_get_tick() - bad_usb->pause_tick);
    }
}

/* A Type Text cut short: the CapsLock of its start is undone, once. With the
 * time a key takes on both sides, as a tap right after another key can be lost. */
static void text_on_abort(BadUsbSender* bad_usb) {
    if(bad_usb->send_cfg.mode == BadUsbModePetit4 || !bad_usb->send_cfg.text_capslock) return;
    if(bad_usb->text_stage != 1) return;
    bad_usb->text_stage = 2;
    furi_delay_ms(bad_usb->send_cfg.text_delay);
    bad_usb->hid->kb_press(bad_usb->hid_inst, HID_KEYBOARD_CAPS_LOCK);
    bad_usb->hid->kb_release(bad_usb->hid_inst, HID_KEYBOARD_CAPS_LOCK);
    furi_delay_ms(bad_usb->send_cfg.text_delay);
}

/* One character typed, and the time until the next one. */
static int32_t text_execute_next(BadUsbSender* bad_usb, File* file) {
    if(bad_usb->text_stage == 0) { // Start
        bad_usb->text_stage = 1;
        bad_usb->st.line_cur = 1;
        if(bad_usb->send_cfg.text_capslock) {
            bad_usb->hid->kb_press(bad_usb->hid_inst, HID_KEYBOARD_CAPS_LOCK);
            bad_usb->hid->kb_release(bad_usb->hid_inst, HID_KEYBOARD_CAPS_LOCK);
            return bad_usb->send_cfg.text_delay;
        }
    }
    if(bad_usb->text_stage == 2) return SEND_STATE_END;

    Petit4IndentKey owed = petit4_indent_key(&bad_usb->text_indent);
    if(owed != Petit4IndentNone) {
        uint16_t press = owed == Petit4IndentBackspace ? HID_KEYBOARD_DELETE :
                                                         HID_KEYBOARD_SPACEBAR;
        bad_usb->text_done++;
        bad_usb->hid->kb_press(bad_usb->hid_inst, press);
        bad_usb->hid->kb_release(bad_usb->hid_inst, press);
        return bad_usb->send_cfg.text_delay;
    }

    uint8_t c;
    if(bad_usb->text_held) {
        bad_usb->text_held = false;
        c = bad_usb->text_byte;
    } else if(storage_file_read(file, &c, 1) == 1) {
        if(!petit4_indent_byte(&bad_usb->text_indent, c)) return 0;
        bad_usb->text_byte = c;
        bad_usb->text_held = true;
        return 0;
    } else if(!bad_usb->text_ended) {
        bad_usb->text_ended = true;
        petit4_indent_end(&bad_usb->text_indent);
        return 0;
    } else { // End
        bad_usb->text_stage = 2;
        if(bad_usb->send_cfg.text_capslock) {
            bad_usb->hid->kb_press(bad_usb->hid_inst, HID_KEYBOARD_CAPS_LOCK);
            bad_usb->hid->kb_release(bad_usb->hid_inst, HID_KEYBOARD_CAPS_LOCK);
            return bad_usb->send_cfg.text_delay;
        }
        return SEND_STATE_END;
    }

    uint16_t key = HID_KEYBOARD_NONE;
    if(c == '\n') {
        key = HID_KEYBOARD_RETURN;
        if(bad_usb->st.line_cur < bad_usb->st.line_nb) bad_usb->st.line_cur++;
    } else if(c == '\t') {
        key = HID_KEYBOARD_TAB;
    } else if(c >= ' ' && c < 0x7F) {
        key = BADUSB_ASCII_TO_KEY(bad_usb, c);
    }
    // A keyboard cannot type the rest: CR of CRLF, and anything beyond ASCII
    if(key == HID_KEYBOARD_NONE) return 0;

    bad_usb->text_done++;
    bad_usb->hid->kb_press(bad_usb->hid_inst, key);
    bad_usb->hid->kb_release(bad_usb->hid_inst, key);
    return bad_usb->send_cfg.text_delay;
}

static int32_t send_execute_next(BadUsbSender* bad_usb, File* file) {
    int32_t delay = bad_usb->send_cfg.mode == BadUsbModePetit4 ? petit4_execute_next(bad_usb) :
                                                                 text_execute_next(bad_usb, file);
    send_update_remaining(bad_usb);
    return delay;
}

static uint32_t bad_usb_flags_get(uint32_t flags_mask, uint32_t timeout) {
    uint32_t flags = furi_thread_flags_get();
    furi_check((flags & FuriFlagError) == 0);
    if(flags == 0) {
        flags = furi_thread_flags_wait(flags_mask, FuriFlagWaitAny, timeout);
        furi_check(((flags & FuriFlagError) == 0) || (flags == (unsigned)FuriFlagErrorTimeout));
    } else {
        uint32_t state = furi_thread_flags_clear(flags);
        furi_check((state & FuriFlagError) == 0);
    }
    return flags;
}

static int32_t bad_usb_worker(void* context) {
    BadUsbSender* bad_usb = context;

    BadUsbWorkerState worker_state = BadUsbStateInit;
    int32_t delay_val = 0;

    FURI_LOG_I(WORKER_TAG, "Init");
    File* send_file = storage_file_alloc(furi_record_open(RECORD_STORAGE));
    bad_usb->st.elapsed = 0;

    while(1) {
        uint32_t start = furi_get_tick();
        if(worker_state == BadUsbStateInit) { // State: initialization
            start = 0;
            FURI_LOG_D(WORKER_TAG, "init start");
            if(storage_file_open(
                   send_file,
                   furi_string_get_cstr(bad_usb->file_path),
                   FSAM_READ,
                   FSOM_OPEN_EXISTING)) {
                if((send_preload(bad_usb, send_file)) && (bad_usb->st.line_nb > 0)) {
                    if(bad_usb->hid->is_connected(bad_usb->hid_inst)) {
                        worker_state = BadUsbStateIdle; // Ready to run
                    } else {
                        worker_state = BadUsbStateNotConnected; // Not connected
                    }
                } else {
                    worker_state = BadUsbStateSendError; // Preload error
                    if(bad_usb->st.error[0] == '\0') {
                        strlcpy(bad_usb->st.error, "Empty file", sizeof(bad_usb->st.error));
                    }
                }
            } else {
                FURI_LOG_E(WORKER_TAG, "File open error");
                worker_state = BadUsbStateFileError; // File open error
            }
            bad_usb->st.state = worker_state;
            FURI_LOG_D(WORKER_TAG, "init done");

        } else if(worker_state == BadUsbStateNotConnected) { // State: Not connected
            start = 0;
            FURI_LOG_D(WORKER_TAG, "not connected wait");
            uint32_t flags = bad_usb_flags_get(
                WorkerEvtEnd | WorkerEvtConnect | WorkerEvtDisconnect | WorkerEvtStartStop,
                FuriWaitForever);
            FURI_LOG_D(WORKER_TAG, "not connected flags: %lu", flags);

            if(flags & WorkerEvtEnd) {
                break;
            } else if(flags & WorkerEvtConnect) {
                worker_state = BadUsbStateIdle; // Ready to run
            } else if(flags & WorkerEvtStartStop) {
                worker_state = BadUsbStateWillRun; // Will run when connected
            }
            bad_usb->st.state = worker_state;

        } else if(worker_state == BadUsbStateIdle) { // State: ready to start
            start = 0;
            FURI_LOG_D(WORKER_TAG, "idle wait");
            uint32_t flags = bad_usb_flags_get(
                WorkerEvtEnd | WorkerEvtStartStop | WorkerEvtDisconnect, FuriWaitForever);
            FURI_LOG_D(WORKER_TAG, "idle flags: %lu", flags);

            if(flags & WorkerEvtEnd) {
                break;
            } else if(flags & WorkerEvtStartStop) { // Start sending
                dolphin_deed(DolphinDeedBadUsbPlayScript);
                delay_val = 0;
                bad_usb->st.line_cur = 0;
                storage_file_seek(send_file, 0, true);
                send_restart(bad_usb);
                worker_state = BadUsbStateRunning;
                bad_usb->st.elapsed = 0;
            } else if(flags & WorkerEvtDisconnect) {
                worker_state = BadUsbStateNotConnected; // Disconnected
            }
            bad_usb->st.state = worker_state;

        } else if(worker_state == BadUsbStateWillRun) { // State: start on connection
            start = 0;
            FURI_LOG_D(WORKER_TAG, "will run wait");
            uint32_t flags = bad_usb_flags_get(
                WorkerEvtEnd | WorkerEvtConnect | WorkerEvtStartStop, FuriWaitForever);
            FURI_LOG_D(WORKER_TAG, "will run flags: %lu", flags);

            if(flags & WorkerEvtEnd) {
                break;
            } else if(flags & WorkerEvtConnect) { // Start sending
                dolphin_deed(DolphinDeedBadUsbPlayScript);
                delay_val = 0;
                bad_usb->st.line_cur = 0;
                storage_file_seek(send_file, 0, true);
                send_restart(bad_usb);
                // extra time for PC to recognize Flipper as keyboard
                flags = furi_thread_flags_wait(
                    WorkerEvtEnd | WorkerEvtDisconnect | WorkerEvtStartStop,
                    FuriFlagWaitAny | FuriFlagNoClear,
                    1500);
                if(flags == (unsigned)FuriFlagErrorTimeout) {
                    // If nothing happened - start sending
                    worker_state = BadUsbStateRunning;
                    bad_usb->st.elapsed = 0;
                } else if(flags & WorkerEvtStartStop) {
                    worker_state = BadUsbStateIdle;
                    furi_thread_flags_clear(WorkerEvtStartStop);
                }
            } else if(flags & WorkerEvtStartStop) { // Cancel scheduled execution
                worker_state = BadUsbStateNotConnected;
            }
            bad_usb->st.state = worker_state;

        } else if(worker_state == BadUsbStateRunning) { // State: running
            FURI_LOG_D(WORKER_TAG, "running");
            uint16_t delay_cur = (delay_val > 100) ? (100) : (delay_val);
            uint32_t flags = furi_thread_flags_wait(
                WorkerEvtEnd | WorkerEvtStartStop | WorkerEvtPauseResume | WorkerEvtDisconnect,
                FuriFlagWaitAny,
                delay_cur);
            FURI_LOG_D(WORKER_TAG, "running flags: %lu", flags);

            delay_val -= delay_cur;
            if(!(flags & FuriFlagError)) {
                if(flags & WorkerEvtEnd) {
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                    text_on_abort(bad_usb);
                    break;
                } else if(flags & WorkerEvtStartStop) {
                    worker_state = BadUsbStateIdle; // Stop sending
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                    text_on_abort(bad_usb);
                } else if(flags & WorkerEvtDisconnect) {
                    worker_state = BadUsbStateNotConnected; // Disconnected
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                } else if(flags & WorkerEvtPauseResume) {
                    send_on_pause(bad_usb);
                    worker_state = BadUsbStatePaused; // Pause
                }
                bad_usb->st.state = worker_state;
                bad_usb->st.elapsed += (furi_get_tick() - start);
                continue;
            } else if(
                (flags == (unsigned)FuriFlagErrorTimeout) ||
                (flags == (unsigned)FuriFlagErrorResource)) {
                if(delay_val > 0) {
                    bad_usb->st.delay_remain--;
                    bad_usb->st.elapsed += (furi_get_tick() - start);
                    continue;
                }
                bad_usb->st.state = BadUsbStateRunning;
                delay_val = send_execute_next(bad_usb, send_file);
                if(delay_val == SEND_STATE_ERROR) { // Send error
                    delay_val = 0;
                    worker_state = BadUsbStateSendError;
                    bad_usb->st.state = worker_state;
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                    text_on_abort(bad_usb);
                } else if(delay_val == SEND_STATE_END) { // End of file
                    delay_val = 0;
                    worker_state = BadUsbStateIdle;
                    bad_usb->st.state = BadUsbStateDone;
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                    bad_usb->st.elapsed += (furi_get_tick() - start);
                    continue;
                } else if(delay_val > 100) {
                    bad_usb->st.state = BadUsbStateDelay; // Show long delays
                    bad_usb->st.delay_remain = delay_val / 100;
                }
            } else {
                furi_check((flags & FuriFlagError) == 0);
            }
        } else if(worker_state == BadUsbStatePaused) { // State: Paused
            start = 0;
            FURI_LOG_D(WORKER_TAG, "paused wait");
            uint32_t flags = bad_usb_flags_get(
                WorkerEvtEnd | WorkerEvtStartStop | WorkerEvtPauseResume | WorkerEvtDisconnect,
                FuriWaitForever);
            FURI_LOG_D(WORKER_TAG, "paused flags: %lu", flags);
            if(!(flags & FuriFlagError)) {
                if(flags & WorkerEvtEnd) {
                    text_on_abort(bad_usb);
                    break;
                } else if(flags & WorkerEvtStartStop) {
                    worker_state = BadUsbStateIdle; // Stop sending
                    bad_usb->st.state = worker_state;
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                    text_on_abort(bad_usb);
                } else if(flags & WorkerEvtDisconnect) {
                    worker_state = BadUsbStateNotConnected; // Disconnected
                    bad_usb->st.state = worker_state;
                    bad_usb->hid->release_all(bad_usb->hid_inst);
                } else if(flags & WorkerEvtPauseResume) {
                    send_on_resume(bad_usb);
                    if(delay_val > 0) {
                        bad_usb->st.state = BadUsbStateDelay;
                        bad_usb->st.delay_remain = delay_val / 100;
                    } else {
                        bad_usb->st.state = BadUsbStateRunning;
                        delay_val = 0;
                    }
                    worker_state = BadUsbStateRunning; // Resume
                }
                continue;
            }
        } else if(
            (worker_state == BadUsbStateFileError) ||
            (worker_state == BadUsbStateSendError)) { // State: error
            start = 0;
            FURI_LOG_D(WORKER_TAG, "error wait");
            uint32_t flags =
                bad_usb_flags_get(WorkerEvtEnd, FuriWaitForever); // Waiting for exit command
            FURI_LOG_D(WORKER_TAG, "error flags: %lu", flags);

            if(flags & WorkerEvtEnd) {
                break;
            }
        }
        if(start) {
            bad_usb->st.elapsed += (furi_get_tick() - start);
        }
    }

    if(bad_usb->hid_started) {
        bad_usb->hid->set_state_callback(bad_usb->hid_inst, NULL, NULL);
        bad_usb->hid->deinit(bad_usb->hid_inst);
    }
    if(bad_usb->petit4) {
        petit4_free(bad_usb->petit4);
    }

    storage_file_close(send_file);
    storage_file_free(send_file);

    FURI_LOG_I(WORKER_TAG, "End");

    return 0;
}

static void bad_usb_sender_set_default_keyboard_layout(BadUsbSender* bad_usb) {
    furi_assert(bad_usb);
    memset(bad_usb->layout, HID_KEYBOARD_NONE, sizeof(bad_usb->layout));
    memcpy(bad_usb->layout, hid_asciimap, MIN(sizeof(hid_asciimap), sizeof(bad_usb->layout)));
}

BadUsbSender* bad_usb_sender_open(FuriString* file_path, const BadUsbSendConfig* send_cfg) {
    furi_assert(file_path);

    BadUsbSender* bad_usb = malloc(sizeof(BadUsbSender));
    bad_usb->file_path = furi_string_alloc();
    furi_string_set(bad_usb->file_path, file_path);
    bad_usb_sender_set_default_keyboard_layout(bad_usb);

    bad_usb->st.state = BadUsbStateInit;
    bad_usb->st.error[0] = '\0';
    bad_usb->st.remain_s = PETIT4_REMAIN_NONE;
    bad_usb->st.part_count = 0;
    bad_usb->st.part_stage = BadUsbPartStageNone;
    bad_usb->petit4_batch = false;
    bad_usb->send_cfg = *send_cfg;
    bad_usb->petit4 = NULL;
    bad_usb->petit4_sync = BAD_USB_PETIT4_SLOW;
    bad_usb->hid_started = false;
    // Only Petit4Send has a choice of link
    if(send_cfg->mode != BadUsbModePetit4) bad_usb->send_cfg.petit4_link = 0;
    bad_usb->hid = send_cfg->mode != BadUsbModePetit4 ? bad_usb_hid_get_usb() :
                   bad_usb->send_cfg.petit4_link      ? bad_usb_hid_get_petit4_multi() :
                                                        bad_usb_hid_get_petit4();

    bad_usb->thread = furi_thread_alloc_ex("BadUsbWorker", 2048, bad_usb_worker, bad_usb);
    furi_thread_start(bad_usb->thread);
    return bad_usb;
} //-V773

void bad_usb_sender_close(BadUsbSender* bad_usb) {
    furi_assert(bad_usb);
    furi_thread_flags_set(furi_thread_get_id(bad_usb->thread), WorkerEvtEnd);
    furi_thread_join(bad_usb->thread);
    furi_thread_free(bad_usb->thread);
    furi_string_free(bad_usb->file_path);
    free(bad_usb);
}

void bad_usb_sender_set_petit4_sync(BadUsbSender* bad_usb, int16_t sync) {
    furi_assert(bad_usb);
    bad_usb->petit4_sync = sync;
}

int16_t bad_usb_sender_get_petit4_sync(BadUsbSender* bad_usb) {
    furi_assert(bad_usb);
    return bad_usb->petit4_sync;
}

void bad_usb_sender_set_keyboard_layout(BadUsbSender* bad_usb, FuriString* layout_path) {
    furi_assert(bad_usb);

    if((bad_usb->st.state == BadUsbStateRunning) || (bad_usb->st.state == BadUsbStateDelay)) {
        // do not update keyboard layout while sending
        return;
    }

    File* layout_file = storage_file_alloc(furi_record_open(RECORD_STORAGE));
    if(!furi_string_empty(layout_path)) { //-V1051
        if(storage_file_open(
               layout_file, furi_string_get_cstr(layout_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
            uint16_t layout[128];
            if(storage_file_read(layout_file, layout, sizeof(layout)) == sizeof(layout)) {
                memcpy(bad_usb->layout, layout, sizeof(layout));
            }
        }
        storage_file_close(layout_file);
    } else {
        bad_usb_sender_set_default_keyboard_layout(bad_usb);
    }
    storage_file_free(layout_file);
}

void bad_usb_sender_start_stop(BadUsbSender* bad_usb) {
    furi_assert(bad_usb);
    furi_thread_flags_set(furi_thread_get_id(bad_usb->thread), WorkerEvtStartStop);
}

void bad_usb_sender_pause_resume(BadUsbSender* bad_usb) {
    furi_assert(bad_usb);
    furi_thread_flags_set(furi_thread_get_id(bad_usb->thread), WorkerEvtPauseResume);
}

BadUsbState* bad_usb_sender_get_state(BadUsbSender* bad_usb) {
    furi_assert(bad_usb);
    return &(bad_usb->st);
}
