#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>
#include <furi_hal.h>
#include "bad_usb_hid.h"
#include "petit4send.h"

typedef enum {
    BadUsbStateInit,
    BadUsbStateNotConnected,
    BadUsbStateIdle,
    BadUsbStateWillRun,
    BadUsbStateRunning,
    BadUsbStateDelay,
    BadUsbStatePaused,
    BadUsbStateDone,
    BadUsbStateSendError,
    BadUsbStateFileError,
} BadUsbWorkerState;

typedef enum {
    BadUsbPartStageNone,
    BadUsbPartStageSend,
    BadUsbPartStageWait,
    BadUsbPartStageConfirm,
} BadUsbPartStage;

typedef struct {
    BadUsbWorkerState state;
    size_t line_cur;
    size_t line_nb;
    uint32_t delay_remain;
    char error[64];
    uint32_t elapsed;
    uint32_t remain_s; // PETIT4_REMAIN_NONE when there is nothing to show
    uint32_t part_start; // A P4A archive: part_count is 0 for anything else
    uint32_t part_count;
    uint32_t part_cur;
    uint32_t wait_left_s;
    BadUsbPartStage part_stage;
} BadUsbState;

typedef enum {
    BadUsbModeText, // The file's characters, typed
    BadUsbModePetit4, // The file, for P4SEND's USB RECEIVE
    BadUsbModeCount,
} BadUsbMode;

typedef struct {
    BadUsbMode mode;
    uint32_t text_delay; // ms between keys
    bool text_capslock; // CapsLock pressed before and after
    Petit4Type petit4_type;
    Petit4Comp petit4_comp;
    uint32_t petit4_interval; // ms between packets
    uint32_t petit4_start_part; // First part of a P4A archive to send
    uint32_t petit4_part_wait; // Seconds of answering save prompts after a part
    uint32_t petit4_parity; // Packets in a parity group of a single file, or 0
    uint32_t petit4_link; // 0 Classic, or 6 or 7 keyboards
} BadUsbSendConfig;

#define BAD_USB_PETIT4_SLOW   (-1) // No sync key: packets at petit4_interval
#define BAD_USB_PETIT4_DETECT (-2) // Run shows P4SEND the sync key
// Otherwise a sync key, 0 to PETIT4_SYNC_KEYS - 1

typedef struct BadUsbSender BadUsbSender;

BadUsbSender* bad_usb_sender_open(FuriString* file_path, const BadUsbSendConfig* send_cfg);

void bad_usb_sender_close(BadUsbSender* bad_usb);

/** A sync key holds while USB stays connected, so it is not a saved setting. */
void bad_usb_sender_set_petit4_sync(BadUsbSender* bad_usb, int16_t sync);

int16_t bad_usb_sender_get_petit4_sync(BadUsbSender* bad_usb);

void bad_usb_sender_set_keyboard_layout(BadUsbSender* bad_usb, FuriString* layout_path);

void bad_usb_sender_start(BadUsbSender* bad_usb);

void bad_usb_sender_stop(BadUsbSender* bad_usb);

void bad_usb_sender_start_stop(BadUsbSender* bad_usb);

void bad_usb_sender_pause_resume(BadUsbSender* bad_usb);

BadUsbState* bad_usb_sender_get_state(BadUsbSender* bad_usb);

#ifdef __cplusplus
}
#endif
