#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>
#include <furi_hal.h>
#include "send_worker.h"
#include "bad_usb_hid.h"

#define SEND_STATE_ERROR (-1)
#define SEND_STATE_END   (-2)

#define TEXT_COUNT_BUFFER_LEN 16

#define PETIT4_ONLY_NONE UINT32_MAX

struct BadUsbSender {
    const BadUsbHidApi* hid;
    void* hid_inst;
    FuriThread* thread;
    BadUsbState st;

    FuriString* file_path;
    uint16_t layout[128];

    BadUsbSendConfig send_cfg;
    bool hid_started;
    Petit4Send* petit4;
    volatile int16_t petit4_sync;
    Petit4Packet petit4_pending;
    bool petit4_has_pending;
    bool petit4_started;
    uint32_t petit4_next_ms;
    uint32_t petit4_last_step;
    uint8_t text_stage;
    uint32_t text_size;
    uint32_t text_done;
    Petit4Indent text_indent;
    uint8_t text_byte;
    bool text_held; // text_byte waits for the keys it owes
    bool text_ended;

    bool petit4_batch;
    Petit4Run petit4_run;
    volatile uint32_t petit4_only_next; // Send Part's part, or PETIT4_ONLY_NONE for Run
    uint32_t pause_tick;
    uint32_t send_mark; // st.elapsed when a single file began to go
    bool send_marked;
    uint32_t remain_tick;
};

#ifdef __cplusplus
}
#endif
