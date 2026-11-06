#pragma once

/* PETIT4SEND wire format: what P4SEND122.PRG reads from a USB keyboard and
 * mouse. Free of Flipper APIs, so that it can be tested on a host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    Petit4TypeAuto,
    Petit4TypeTxt,
    Petit4TypeDat,
    Petit4TypeGrp, /* from an uncompressed 24 or 32-bit BMP */
    Petit4TypeCount,
} Petit4Type;

typedef enum {
    Petit4CompAuto,
    Petit4CompLzss,
    Petit4CompNone,
    Petit4CompCount,
} Petit4Comp;

/** One state of the keyboard and mouse, in USB HID terms. */
typedef struct {
    uint8_t mods;
    uint8_t keys[6];
    uint8_t mouse;
} Petit4Packet;

typedef size_t (*Petit4ReadFn)(void* context, uint8_t* buf, size_t len);
typedef bool (*Petit4SeekFn)(void* context, uint32_t offset);

typedef struct Petit4Send Petit4Send;

Petit4Send* petit4_alloc(Petit4ReadFn read, Petit4SeekFn seek, void* context);

void petit4_free(Petit4Send* p);

/** Reads the whole file once for its length, CRC and compressed size.
 * @return false if the file cannot be sent, for the reason petit4_error gives */
bool petit4_prepare(Petit4Send* p, const char* file_name, Petit4Type type, Petit4Comp comp);

/** Goes back to the first packet. */
bool petit4_restart(Petit4Send* p);

/** After petit4_prepare: a stream with parity groups of that many packets, or 0 for an
 * ordinary one. Only P4SENDX reads parity, P4SEND does not. */
bool petit4_set_parity(Petit4Send* p, uint8_t parity);

/** @return false when every packet has been given */
bool petit4_next_packet(Petit4Send* p, Petit4Packet* packet);

/* The NS2 link, which only P4SENDX reads. docs/protocol.md has the format. */
#define PETIT4_MULTI_KEYBOARDS   7
#define PETIT4_MULTI_LEAD        4
#define PETIT4_MULTI_RATE_PER_MS 15258 /* stream bytes a second at 1 ms a packet, with G 32 */
#define PETIT4_MULTI_PARITY      32
#define PETIT4_MULTI_CONFIG_SIZE (9 + PETIT4_MULTI_KEYBOARDS * 25)

/** The state of all 7 keyboards. The clock is in mods, on the first. */
typedef struct {
    uint8_t mods;
    uint8_t keys[PETIT4_MULTI_KEYBOARDS][6];
} Petit4MultiPacket;

/** A stream on this link has parity: 0 becomes PETIT4_MULTI_PARITY. Stays until changed,
 * through petit4_prepare and petit4_part_begin. Takes effect at the next petit4_restart. */
void petit4_set_multi(Petit4Send* p, uint8_t keyboards);

/** @return false when every packet has been given */
bool petit4_next_multi(Petit4Send* p, Petit4MultiPacket* packet);

/** Enter taps and such: on the first keyboard, the others empty. */
void petit4_multi_from_packet(const Petit4Packet* from, Petit4MultiPacket* to);

/** The USB configuration descriptor of 7 boot keyboards, and each one's report descriptor. */
void petit4_multi_config_desc(uint8_t* out, uint8_t keyboards);
const uint8_t* petit4_multi_report_desc(uint16_t* size);

/** Bytes of the stream used so far and in all, for showing progress. */
size_t petit4_position(const Petit4Send* p);
size_t petit4_total(const Petit4Send* p);

/** The name SmileBASIC 4 will save the file under, such as TXT:HELLO. */
const char* petit4_name(const Petit4Send* p);

/** Why petit4_prepare failed. */
const char* petit4_error(const Petit4Send* p);

/* ---- P4A batch: parts split on the PC, each a stream with parity or an ordinary one. ---- */

#define PETIT4_BATCH_HEAD_SIZE 24
#define PETIT4_BATCH_ROW_SIZE  16
#define PETIT4_BATCH_MAX_PARTS 100000 /* part names hold 5 digits */

/* No remaining time to show. */
#define PETIT4_REMAIN_NONE UINT32_MAX

typedef struct {
    uint32_t id;
    uint32_t part_size;
    uint32_t part_count;
    uint32_t image_size;
    uint32_t file_size;
    uint32_t parity; /* packets in a parity group, or 0 */
} Petit4Batch;

typedef struct {
    uint32_t offset;
    uint32_t sent;
    uint32_t original;
    uint16_t crc;
    uint8_t comp;
} Petit4PartRow;

/** Whether a file starting with these bytes is an archive, of any version. */
bool petit4_batch_is_p4a(const uint8_t* head, size_t len);

/** Reads an archive's header.
 * @return NULL, or why it cannot be sent. part_count is set for a Start Part
 *         past the end, so that the message can tell the limit. */
const char* petit4_batch_parse(
    Petit4Batch* batch,
    const uint8_t* head,
    size_t len,
    uint64_t file_size,
    uint32_t start_part);

/** Reads a table row and checks it against the file and the part.
 * @return NULL, or what is wrong with it */
const char* petit4_part_row_parse(
    Petit4PartRow* row,
    const Petit4Batch* batch,
    uint32_t part,
    const uint8_t* raw);

/** All packets of a part's stream with parity, its end included. */
uint32_t petit4_part_packets(uint32_t parity, uint32_t sent);
uint32_t petit4_part_packets_multi(uint8_t keyboards, uint32_t parity, uint32_t sent);

/** Reads a part's row and makes ready to send the stored bytes as they are.
 * @return false for the reason petit4_error gives */
bool petit4_part_begin(
    Petit4Send* p,
    const Petit4Batch* batch,
    uint32_t part,
    Petit4PartRow* row);

/* Type Text into SB4's editor. After Enter the editor starts the next line with the
 * last line's indent, as spaces that Backspace takes one by one. So a line's leading
 * whitespace is counted, not typed, and only the difference is sent before the rest.
 * A tab counts as this many spaces: a guess, as the manual gives no default TABSTEP. */
#define PETIT4_TAB_STEP 1

typedef struct {
    uint32_t indent; /* spaces the editor has on this line */
    uint32_t lead; /* this line's leading whitespace in the file so far */
    uint32_t backspaces; /* keys owed before the byte */
    uint32_t spaces;
    bool at_start; /* still in the leading whitespace */
} Petit4Indent;

typedef enum {
    Petit4IndentNone,
    Petit4IndentBackspace,
    Petit4IndentSpace,
} Petit4IndentKey;

void petit4_indent_init(Petit4Indent* in);
/* One byte of the file: true if it is to be typed, after the keys it owes. */
bool petit4_indent_byte(Petit4Indent* in, uint8_t c);
/* The file's end: a last line owes its indent too. */
void petit4_indent_end(Petit4Indent* in);
/* The next key owed, if any. */
Petit4IndentKey petit4_indent_key(Petit4Indent* in);

/* An Enter tap every 500 ms, held 50 ms. A press is at least the spacing after
 * the one before, and over by the time the quiet part of a wait begins. */
#define PETIT4_TAP_SPACING_MS 500
#define PETIT4_TAP_HOLD_MS    50
#define PETIT4_TAP_QUIET_MS   1000

typedef enum {
    Petit4TapNone,
    Petit4TapPress,
    Petit4TapRelease,
} Petit4TapAction;

typedef struct {
    uint32_t last_press;
    uint32_t quiet_from;
    bool forever;
    bool down;
} Petit4Tap;

/** The first press comes a spacing after now, as a dialog takes time to open. */
void petit4_tap_start(Petit4Tap* tap, uint32_t now, uint32_t wait_end, bool forever);

Petit4TapAction petit4_tap_next(const Petit4Tap* tap, uint32_t now);

/** To be called with the time after the packet has gone. */
void petit4_tap_done(Petit4Tap* tap, Petit4TapAction action, uint32_t now);

/** The keyboard state for a press or a release: nothing else held, no mouse. */
void petit4_tap_packet(Petit4TapAction action, Petit4Packet* packet);

typedef enum {
    Petit4RunStageSend, /* the worker gives the stream's packets */
    Petit4RunStageWait, /* tapping until the next part */
    Petit4RunStageConfirm, /* all sent: tapping until stopped */
} Petit4RunStage;

/** The order of parts and waits. Sending packets stays with the caller. */
typedef struct {
    Petit4Batch batch;
    uint32_t start_part;
    uint32_t part; /* being sent, or next while waiting */
    uint32_t wait_ms;
    uint32_t wait_end;
    uint32_t sent_from_start; /* bytes stored for the parts from start_part on */
    uint32_t sent_left; /* the same from part on */
    uint32_t sent_cur;
    Petit4RunStage stage;
    Petit4Tap tap;
    uint8_t multi;
} Petit4Run;

typedef enum {
    Petit4RunEventIdle, /* nothing to do for *wait ms */
    Petit4RunEventTapPress, /* send *packet, then call petit4_tap_done */
    Petit4RunEventTapRelease, /* the same */
    Petit4RunEventStartStream, /* the part is ready: send it */
    Petit4RunEventError, /* petit4_error tells why */
} Petit4RunEvent;

void petit4_run_init(
    Petit4Run* run,
    const Petit4Batch* batch,
    uint32_t start_part,
    uint32_t wait_ms);

/** The NS2 link: before petit4_run_open. */
void petit4_run_set_multi(Petit4Run* run, uint8_t keyboards);

/** Checks every row of the table, so that a bad file shows before hours are
 * spent, and makes the Start Part ready.
 * @return false for the reason petit4_error gives */
bool petit4_run_open(Petit4Run* run, Petit4Send* p);

/** Run was pressed: from the Start Part again, at the next petit4_run_step. */
void petit4_run_restart(Petit4Run* run, uint32_t now);

/** The stream of the current part has ended. */
void petit4_run_part_sent(Petit4Run* run, uint32_t now);

/** One thing of the wait or confirm stage. No data may go out in these. */
Petit4RunEvent
    petit4_run_step(Petit4Run* run, Petit4Send* p, uint32_t now, Petit4Packet* packet, int32_t* wait);

/** A pause does not use up the wait. */
void petit4_run_shift(Petit4Run* run, uint32_t ms);

/** Whole seconds until the next part goes, in the wait stage. */
uint32_t petit4_run_wait_left_s(const Petit4Run* run, uint32_t now);

/** Single files: seconds to go from bytes left at the rate so far, or at
 * fixed_rate bytes a second until 5 s have been spent sending. */
uint32_t petit4_remaining_s(uint32_t left, uint32_t done, uint32_t send_ms, uint32_t fixed_rate);

/** A batch knows all its lengths: packets left, each taking packet_us, and the
 * waits to come. PETIT4_REMAIN_NONE when confirming. */
uint32_t petit4_run_remaining_s(
    const Petit4Run* run,
    const Petit4Send* p,
    uint32_t now,
    uint32_t packet_us);

/** The rate to expect before any is measured, in stream bytes a second: 37 bits
 * a packet and 5 more every other one. */
uint32_t petit4_slow_rate(uint32_t interval_ms);
#define PETIT4_FAST_RATE 296

/* Fast mode sends a packet with every VSYNC. */
#define PETIT4_FAST_PACKET_US 16667

/* Fast mode, in the time of a clock the USB host keeps, in ms. As in the
 * Petit4Send firmware, a counter of 8 ms steps runs round 25 values in 200 ms,
 * which is 12 VSYNCs, and a packet goes out at every odd distance from the
 * sync key that P4SEND's DETECT SYNC KEY shows. */
#define PETIT4_SYNC_KEYS 25

/** When the next packet may go, after the current 8 ms step. */
uint32_t petit4_fast_next(uint32_t ms, uint8_t sync);

/** As petit4_fast_next, for a packet sent at ms, however late that was: the
 * next one must not reach the same VSYNC. */
uint32_t petit4_fast_after_send(uint32_t ms, uint8_t sync);

/** The key to hold at ms while the receiver detects the sync key. */
uint8_t petit4_detect_key(uint32_t ms);

#ifdef __cplusplus
}
#endif
