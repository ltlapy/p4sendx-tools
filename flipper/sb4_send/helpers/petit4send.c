#include "petit4send.h"

#include <stdlib.h>
#include <string.h>

/* The stream, from the PC program of Petit4Send 1.2.2:
 *   16 zeros, 50 ones, a zero: the receiver finds the start with them
 *   type (4 bytes, such as "TXT:"), name (32), file length (4)
 *   image width and height (2 and 2), compression (1)
 *   transfer length (4), the data, CRC of the uncompressed file (2)
 *   16 zeros
 * Numbers are little endian. */
#define HEADER_LEADER (16 + 50 + 1)
#define HEADER_SIZE   (HEADER_LEADER + 4 + 32 + 4 + 4 + 1 + 4)
#define TAIL_ZEROS    16
#define TAIL_SIZE     (2 + TAIL_ZEROS)

#define NAME_SIZE 32

#define CRC_POLY 0xC7ED

/* LZSS as P4SEND's UNLZ with WLB 10, MLB 5, BIT 8 reads it, bits from the
 * lowest up: 0 and a byte, or 1, distance - 1 (10 bits), length - 1 (5). */
#define LZ_MIN_MATCH  3
#define LZ_MAX_MATCH  32
#define LZ_MAX_DIST   1023
#define LZ_RING_SIZE  2048
#define LZ_RING_MASK  (LZ_RING_SIZE - 1)
#define LZ_HASH_SIZE  1024
#define LZ_PREV_SIZE  1024
#define LZ_MAX_CHAIN  64

/* A packet carries 36 bits as the choice of 6 keys of KEYS_COUNT and one as
 * Alt; every other packet adds 5 bits as mouse buttons. */
#define KEYS_COUNT  195
#define KEYS_HELD   6
#define PACKET_BITS 37
#define MOUSE_BITS  5
#define MASK_36     ((1ULL << 36) - 1)

#define FAST_STEP_MS        8
#define FAST_MIN_SPACING_MS 12

#define BMP_HEADER_SIZE 70
#define GRP_MAX_SIZE    2048

/* The NS2 link: keys of KEYS_COUNT without these, in order. */
#define MULTI_KEYS_EACH  27
#define MULTI_KEYS_ALL   (PETIT4_MULTI_KEYBOARDS * MULTI_KEYS_EACH)
#define MULTI_VALUE_BITS 18
#define MULTI_SAME       (1UL << MULTI_VALUE_BITS)
#define MULTI_MARK       0x2B4A5

#define HID_MOD_SHIFT 0x02
#define HID_MOD_ALT   0x04
#define HID_MOD_GUI   0x08

/* Parity streams, for batch parts. The leader is the usual one, then G, then zero bits
 * to the end of the odd packet that ends at bit 553, so that groups start at an even
 * packet. In a group of G packets the last one is the XOR of the others. */
#define PARITY_LEAD_BYTES   (HEADER_LEADER + 1)
#define PARITY_LEAD_PACKETS 14
#define PARITY_TAIL_SIZE    2

/* After the stream, clock values with no keys held, as the PC program ends. */
static const uint8_t end_mods[] = {
    HID_MOD_GUI | HID_MOD_SHIFT,
    HID_MOD_SHIFT,
    HID_MOD_GUI,
    0,
};

/* KEYCODE% of P4SEND122.PRG, in its order. */
static const uint8_t keycodes[KEYS_COUNT] = {
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,
    38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  54,  55,
    56,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  84,  85,  86,  87,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 103, 104, 105, 106, 107, 108,
    109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 128, 129, 133, 134, 135, 137, 155, 156, 157, 158, 159, 160, 161, 162, 163,
    164, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208,
    209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 165, 166, 167, 168,
    169, 170, 171, 172, 173, 174, 175, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 249,
};

typedef enum {
    PhaseHeader,
    PhaseData,
    PhaseTail,
    PhaseEnd,
} Phase;

struct Petit4Send {
    Petit4ReadFn read;
    Petit4SeekFn seek;
    void* context;

    /* The file. */
    uint8_t file_buf[256];
    size_t file_pos;
    size_t file_len;
    bool is_text;
    bool bom_checked;
    uint8_t text_out[4];
    uint8_t text_out_pos;
    uint8_t text_out_len;
    const char* error;

    /* A part of an archive: bytes of the file, sent as they are. */
    bool is_window;
    uint32_t window_offset;
    uint32_t window_size;
    uint32_t window_left;

    /* A BMP, given as SmileBASIC 4's 32-bit colours from the top row down. */
    bool is_image;
    uint32_t width;
    uint32_t height;
    uint32_t data_offset;
    uint32_t stride;
    bool top_down;
    uint8_t pixel_bytes;
    bool has_alpha;
    uint32_t image_x;
    uint32_t image_y;
    uint8_t pixel[4];
    uint8_t pixel_pos;

    /* Measured by petit4_prepare. */
    bool measuring;
    uint32_t file_size;
    uint16_t crc;
    bool compress;
    uint32_t transfer_size;

    /* LZSS. */
    uint8_t ring[LZ_RING_SIZE];
    uint32_t head[LZ_HASH_SIZE];
    uint32_t prev[LZ_PREV_SIZE];
    uint32_t lz_cur;
    uint32_t lz_end;
    bool lz_eof;
    uint32_t lz_bits;
    uint8_t lz_bit_count;

    /* The stream. */
    char name[4 + NAME_SIZE + 1];
    uint8_t header[HEADER_SIZE];
    uint8_t tail[TAIL_SIZE];
    Phase phase;
    size_t phase_pos;
    size_t position;

    /* Packets. */
    uint64_t choose[KEYS_HELD + 1][KEYS_COUNT];
    /* The NS2 link. */
    uint8_t multi; /* keyboards in use, or 0 */
    uint8_t multi_keys[MULTI_KEYS_ALL];
    uint32_t multi_xor[PETIT4_MULTI_KEYBOARDS];
    uint32_t multi_last[PETIT4_MULTI_KEYBOARDS];
    uint64_t bits;
    int bit_count;
    bool stream_done;
    uint32_t clock;
    uint8_t mouse;
    uint8_t end_step;

    /* A parity stream, or 0 for an ordinary one. */
    uint8_t parity;
    uint8_t group_pos;
    uint32_t lead_pos;
    uint64_t xor_value;
    uint8_t xor_mouse;
};

static int file_next(Petit4Send* p) {
    if(p->file_pos >= p->file_len) {
        p->file_len = p->read(p->context, p->file_buf, sizeof(p->file_buf));
        p->file_pos = 0;
        if(p->file_len == 0) return -1;
    }
    return p->file_buf[p->file_pos++];
}

static int file_peek(Petit4Send* p) {
    int c = file_next(p);
    if(c >= 0) p->file_pos--;
    return c;
}

/* The next code point of a UTF-8 file, or -1. */
static int32_t utf8_next(Petit4Send* p) {
    int c = file_next(p);
    if(c < 0) return -1;
    if(c < 0x80) return c;

    int more;
    int32_t code;
    int32_t least;
    if(c >= 0xC2 && c <= 0xDF) {
        more = 1;
        code = c & 0x1F;
        least = 0x80;
    } else if(c >= 0xE0 && c <= 0xEF) {
        more = 2;
        code = c & 0x0F;
        least = 0x800;
    } else if(c >= 0xF0 && c <= 0xF4) {
        more = 3;
        code = c & 0x07;
        least = 0x10000;
    } else {
        return 0xFFFD;
    }
    for(; more > 0; more--) {
        int next = file_peek(p);
        if(next < 0 || (next & 0xC0) != 0x80) return 0xFFFD;
        file_next(p);
        code = (code << 6) | (next & 0x3F);
    }
    if(code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0xFFFD;
    return code;
}

/* TXT: files travel as UTF-16, the way SmileBASIC 4 holds strings. */
static int text_next(Petit4Send* p) {
    if(p->text_out_pos >= p->text_out_len) {
        int32_t code = utf8_next(p);
        if(!p->bom_checked) {
            p->bom_checked = true;
            if(code == 0xFEFF) code = utf8_next(p);
        }
        if(code < 0) return -1;
        p->text_out_pos = 0;
        if(code >= 0x10000) {
            code -= 0x10000;
            uint16_t high = 0xD800 | (code >> 10);
            uint16_t low = 0xDC00 | (code & 0x3FF);
            p->text_out[0] = high & 0xFF;
            p->text_out[1] = high >> 8;
            p->text_out[2] = low & 0xFF;
            p->text_out[3] = low >> 8;
            p->text_out_len = 4;
        } else {
            p->text_out[0] = code & 0xFF;
            p->text_out[1] = code >> 8;
            p->text_out_len = 2;
        }
    }
    return p->text_out[p->text_out_pos++];
}

static uint32_t read_u32(const uint8_t* from) {
    return from[0] | (from[1] << 8) | (from[2] << 16) | ((uint32_t)from[3] << 24);
}

static bool image_parse(Petit4Send* p) {
    uint8_t h[BMP_HEADER_SIZE];
    memset(h, 0, sizeof(h));
    for(size_t i = 0; i < sizeof(h); i++) {
        int c = file_next(p);
        if(c < 0) {
            if(i < 54) {
                p->error = "Not a BMP";
                return false;
            }
            break;
        }
        h[i] = c;
    }
    if(h[0] != 'B' || h[1] != 'M') {
        p->error = "Not a BMP";
        return false;
    }
    uint32_t dib_size = read_u32(h + 14);
    int32_t width = (int32_t)read_u32(h + 18);
    int32_t height = (int32_t)read_u32(h + 22);
    uint16_t bits = h[28] | (h[29] << 8);
    uint32_t compression = read_u32(h + 30);

    bool masks_fine = read_u32(h + 54) == 0x00FF0000 && read_u32(h + 58) == 0x0000FF00 &&
                      read_u32(h + 62) == 0x000000FF;
    if(!((compression == 0 && (bits == 24 || bits == 32)) ||
         (compression == 3 && bits == 32 && masks_fine))) {
        p->error = "Need 24/32-bit BMP";
        return false;
    }
    p->top_down = height < 0;
    if(height < 0) height = -height;
    if(width <= 0 || height <= 0 || width > GRP_MAX_SIZE || height > GRP_MAX_SIZE) {
        p->error = "BMP over 2048x2048";
        return false;
    }
    p->width = width;
    p->height = height;
    p->data_offset = read_u32(h + 10);
    p->pixel_bytes = bits / 8;
    p->stride = ((uint32_t)width * p->pixel_bytes + 3) & ~3UL;
    /* Plain 32-bit BMPs leave that byte unused, usually zero. */
    p->has_alpha = compression == 3 && dib_size >= 56 && read_u32(h + 66) == 0xFF000000;
    return true;
}

static int image_next(Petit4Send* p) {
    if(p->pixel_pos < sizeof(p->pixel)) return p->pixel[p->pixel_pos++];
    if(p->image_y >= p->height) return -1;

    if(p->image_x == 0) {
        uint32_t row = p->top_down ? p->image_y : p->height - 1 - p->image_y;
        if(!p->seek(p->context, p->data_offset + row * p->stride)) return -1;
        p->file_pos = 0;
        p->file_len = 0;
    }
    for(uint8_t i = 0; i < p->pixel_bytes; i++) {
        int c = file_next(p);
        if(c < 0) {
            p->error = "BMP is cut short";
            p->image_y = p->height;
            return -1;
        }
        p->pixel[i] = c;
    }
    if(!p->has_alpha) p->pixel[3] = 0xFF;
    if(++p->image_x >= p->width) {
        p->image_x = 0;
        p->image_y++;
    }
    p->pixel_pos = 1;
    return p->pixel[0];
}

static uint16_t crc_add(uint16_t crc, uint8_t byte) {
    uint16_t t = (crc ^ byte) & 0xFF;
    for(int i = 0; i < 8; i++) {
        t = (t & 1) ? (CRC_POLY ^ (t >> 1)) : (t >> 1);
    }
    return t ^ (crc >> 8);
}

static int window_next(Petit4Send* p) {
    if(p->window_left == 0) return -1;
    int c = file_next(p);
    if(c >= 0) p->window_left--;
    return c;
}

static int source_next(Petit4Send* p) {
    int c = p->is_window ? window_next(p) :
            p->is_image  ? image_next(p) :
            p->is_text   ? text_next(p) :
                           file_next(p);
    if(c >= 0 && p->measuring) {
        p->file_size++;
        p->crc = crc_add(p->crc, c);
    }
    return c;
}

static bool source_rewind(Petit4Send* p) {
    p->file_pos = 0;
    p->file_len = 0;
    p->bom_checked = false;
    p->text_out_pos = 0;
    p->text_out_len = 0;
    p->image_x = 0;
    p->image_y = 0;
    p->pixel_pos = sizeof(p->pixel);
    if(p->is_window) {
        p->window_left = p->window_size;
        return p->seek(p->context, p->window_offset);
    }
    return p->seek(p->context, 0);
}

static void lz_reset(Petit4Send* p) {
    memset(p->head, 0, sizeof(p->head));
    memset(p->prev, 0, sizeof(p->prev));
    p->lz_cur = 0;
    p->lz_end = 0;
    p->lz_eof = false;
    p->lz_bits = 0;
    p->lz_bit_count = 0;
}

static uint32_t lz_hash(const Petit4Send* p, uint32_t at) {
    const uint8_t* ring = p->ring;
    return ((ring[at & LZ_RING_MASK] << 4) ^ (ring[(at + 1) & LZ_RING_MASK] << 2) ^
            ring[(at + 2) & LZ_RING_MASK]) &
           (LZ_HASH_SIZE - 1);
}

static void lz_insert(Petit4Send* p, uint32_t at) {
    if(at + 2 >= p->lz_end) return;
    uint32_t hash = lz_hash(p, at);
    p->prev[at & (LZ_PREV_SIZE - 1)] = p->head[hash];
    p->head[hash] = at + 1;
}

/* The next literal or match goes into lz_bits. False when the file has ended. */
static bool lz_token(Petit4Send* p) {
    while(!p->lz_eof && p->lz_end - p->lz_cur < LZ_MAX_MATCH) {
        int c = source_next(p);
        if(c < 0) {
            p->lz_eof = true;
        } else {
            p->ring[p->lz_end++ & LZ_RING_MASK] = c;
        }
    }
    uint32_t cur = p->lz_cur;
    uint32_t available = p->lz_end - cur;
    if(available == 0) return false;

    uint32_t best_len = 0;
    uint32_t best_dist = 0;
    if(available >= LZ_MIN_MATCH) {
        uint32_t limit = available < LZ_MAX_MATCH ? available : LZ_MAX_MATCH;
        uint32_t candidate = p->head[lz_hash(p, cur)];
        for(int chain = 0; candidate != 0 && chain < LZ_MAX_CHAIN; chain++) {
            uint32_t from = candidate - 1;
            uint32_t dist = cur - from;
            if(dist == 0 || dist > LZ_MAX_DIST) break;
            uint32_t len = 0;
            while(len < limit && p->ring[(from + len) & LZ_RING_MASK] ==
                                     p->ring[(cur + len) & LZ_RING_MASK]) {
                len++;
            }
            if(len > best_len) {
                best_len = len;
                best_dist = dist;
                if(len == limit) break;
            }
            uint32_t next = p->prev[from & (LZ_PREV_SIZE - 1)];
            /* An entry overwritten since then points forward: the chain is over. */
            if(next >= candidate) break;
            candidate = next;
        }
    }

    uint32_t advance;
    if(best_len >= LZ_MIN_MATCH) {
        uint32_t token = 1 | ((best_dist - 1) << 1) | ((best_len - 1) << 11);
        p->lz_bits |= token << p->lz_bit_count;
        p->lz_bit_count += 16;
        advance = best_len;
    } else {
        uint32_t token = (uint32_t)p->ring[cur & LZ_RING_MASK] << 1;
        p->lz_bits |= token << p->lz_bit_count;
        p->lz_bit_count += 9;
        advance = 1;
    }
    for(uint32_t i = 0; i < advance; i++) {
        lz_insert(p, cur + i);
    }
    p->lz_cur = cur + advance;
    return true;
}

static int lz_next(Petit4Send* p) {
    while(p->lz_bit_count < 8) {
        if(!lz_token(p)) {
            if(p->lz_bit_count == 0) return -1;
            p->lz_bit_count = 8;
        }
    }
    int byte = p->lz_bits & 0xFF;
    p->lz_bits >>= 8;
    p->lz_bit_count -= 8;
    return byte;
}

static int data_next(Petit4Send* p) {
    return p->compress ? lz_next(p) : source_next(p);
}

static int stream_next(Petit4Send* p) {
    for(;;) {
        switch(p->phase) {
        case PhaseHeader:
            if(p->phase_pos < HEADER_SIZE) {
                p->position++;
                return p->header[p->phase_pos++];
            }
            p->phase = PhaseData;
            break;
        case PhaseData: {
            int byte = data_next(p);
            if(byte >= 0) {
                p->position++;
                return byte;
            }
            p->phase = PhaseTail;
            p->phase_pos = 0;
            break;
        }
        case PhaseTail:
            if(p->phase_pos < (p->parity ? PARITY_TAIL_SIZE : TAIL_SIZE)) {
                p->position++;
                return p->tail[p->phase_pos++];
            }
            p->phase = PhaseEnd;
            break;
        case PhaseEnd:
        default:
            return -1;
        }
    }
}

static void put_u16(uint8_t* to, uint16_t value) {
    to[0] = value & 0xFF;
    to[1] = value >> 8;
}

static void put_u32(uint8_t* to, uint32_t value) {
    put_u16(to, value & 0xFFFF);
    put_u16(to + 2, value >> 16);
}

static bool has_extension(const char* file_name, const char* extension);

/* SmileBASIC 4 takes a file's whole name, and a new project starts with
 * MAIN.PRG: "dir/main.prg" gives MAIN.PRG, map.bin gives MAP.BIN. Only these
 * say no more than the type does, so "dir/hello.txt" gives HELLO. */
static bool is_dropped_extension(const char* base) {
    static const char* const dropped[] = {"txt", "dat", "bas", "bmp", "png", "jpg", "jpeg"};
    for(size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++) {
        if(has_extension(base, dropped[i])) return true;
    }
    return false;
}

static void make_name(Petit4Send* p, const char* file_name) {
    const char* base = strrchr(file_name, '/');
    base = base ? base + 1 : file_name;
    const char* dot = strrchr(base, '.');
    size_t len = (dot && dot != base && is_dropped_extension(base)) ? (size_t)(dot - base) :
                                                                      strlen(base);
    if(len > NAME_SIZE) len = NAME_SIZE;

    memcpy(p->name, p->is_image ? "GRP:" : p->is_text ? "TXT:" : "DAT:", 4);
    char* name = p->name + 4;
    for(size_t i = 0; i < len; i++) {
        char c = base[i];
        if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
        bool fine = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                    (c == '.' && i > 0);
        name[i] = fine ? c : '_';
    }
    if(len == 0) {
        memcpy(name, "FILE", 4);
        len = 4;
    }
    name[len] = '\0';
}

static bool has_extension(const char* file_name, const char* extension) {
    const char* dot = strrchr(file_name, '.');
    if(!dot) return false;
    const char* a = dot + 1;
    while(*a && *extension && (*a | 0x20) == *extension) {
        a++;
        extension++;
    }
    return !*a && !*extension;
}

static bool is_text_name(const char* file_name) {
    return has_extension(file_name, "txt") || has_extension(file_name, "prg") ||
           has_extension(file_name, "bas");
}

Petit4Send* petit4_alloc(Petit4ReadFn read, Petit4SeekFn seek, void* context) {
    Petit4Send* p = malloc(sizeof(Petit4Send));
    memset(p, 0, sizeof(Petit4Send));
    p->read = read;
    p->seek = seek;
    p->context = context;

    for(int n = 0; n < KEYS_COUNT; n++) {
        p->choose[0][n] = 1;
        for(int k = 1; k <= KEYS_HELD; k++) {
            p->choose[k][n] = n == 0 ? 0 : p->choose[k - 1][n - 1] + p->choose[k][n - 1];
        }
    }
    /* Sorted by usage, leaving out the keys that mean something by themselves. */
    int count = 0;
    for(int usage = 0; usage < 256; usage++) {
        if((usage >= 40 && usage <= 44) || usage == 88) continue;
        for(int n = 0; n < KEYS_COUNT; n++) {
            if(keycodes[n] == usage) p->multi_keys[count++] = usage;
        }
    }
    return p;
}

void petit4_free(Petit4Send* p) {
    free(p);
}

static void build_stream_ends(Petit4Send* p, uint8_t compression) {
    memset(p->header, 0, sizeof(p->header));
    memset(p->header + 16, 1, 50);
    uint8_t* at = p->header + HEADER_LEADER;
    memcpy(at, p->name, 4);
    at += 4;
    strncpy((char*)at, p->name + 4, NAME_SIZE);
    at += NAME_SIZE;
    put_u32(at, p->file_size);
    at += 4;
    put_u16(at, p->width);
    put_u16(at + 2, p->height);
    at += 4;
    *at++ = compression;
    put_u32(at, p->transfer_size);

    memset(p->tail, 0, sizeof(p->tail));
    put_u16(p->tail, p->crc);
}

bool petit4_prepare(Petit4Send* p, const char* file_name, Petit4Type type, Petit4Comp comp) {
    p->parity = 0;
    p->error = "Cannot read file";
    p->is_window = false;
    p->is_image = type == Petit4TypeGrp ||
                  (type == Petit4TypeAuto && has_extension(file_name, "bmp"));
    p->is_text = !p->is_image &&
                 (type == Petit4TypeTxt || (type == Petit4TypeAuto && is_text_name(file_name)));
    make_name(p, file_name);
    p->width = 0;
    p->height = 0;

    if(!source_rewind(p)) return false;
    if(p->is_image) {
        if(!image_parse(p)) return false;
        if(!source_rewind(p)) return false;
    }
    const char* error_before = p->error;
    p->measuring = true;
    p->file_size = 0;
    p->crc = 0xFFFF;
    uint32_t compressed = 0;
    if(comp == Petit4CompNone) {
        while(source_next(p) >= 0) {
        }
    } else {
        lz_reset(p);
        while(lz_next(p) >= 0) {
            compressed++;
        }
    }
    p->measuring = false;
    p->crc ^= 0xFFFF;
    if(p->error != error_before) return false;
    if(p->file_size == 0) {
        p->error = "Empty file";
        return false;
    }

    p->compress = comp == Petit4CompLzss || (comp == Petit4CompAuto && compressed < p->file_size);
    p->transfer_size = p->compress ? compressed : p->file_size;
    build_stream_ends(p, p->compress ? 1 : 0);

    return petit4_restart(p);
}

bool petit4_set_parity(Petit4Send* p, uint8_t parity) {
    p->parity = parity;
    return petit4_restart(p);
}

bool petit4_restart(Petit4Send* p) {
    if(!source_rewind(p)) return false;
    lz_reset(p);
    p->phase = PhaseHeader;
    p->phase_pos = p->parity ? HEADER_LEADER : 0;
    p->position = 0;
    p->group_pos = 0;
    p->lead_pos = 0;
    memset(p->multi_last, 0, sizeof(p->multi_last));
    p->xor_value = 0;
    p->xor_mouse = 0;
    memset(p->multi_xor, 0, sizeof(p->multi_xor));
    p->bits = 0;
    p->bit_count = 0;
    p->stream_done = false;
    p->clock = 0;
    p->mouse = 0;
    p->end_step = 0;
    return true;
}

static void fill_bits(Petit4Send* p, int want) {
    while(p->bit_count < want && !p->stream_done) {
        int byte = stream_next(p);
        if(byte < 0) {
            p->stream_done = true;
        } else {
            p->bits |= (uint64_t)byte << p->bit_count;
            p->bit_count += 8;
        }
    }
}

static uint64_t take_bits(Petit4Send* p, int count) {
    uint64_t value = p->bits & ((1ULL << count) - 1);
    p->bits >>= count;
    p->bit_count -= count;
    if(p->bit_count < 0) p->bit_count = 0;
    return value;
}

static void choose_keys(const Petit4Send* p, uint64_t value, uint8_t* keys) {
    int index = KEYS_COUNT - 1;
    for(int k = KEYS_HELD; k >= 1; k--) {
        while(p->choose[k][index] > value) {
            index--;
        }
        value -= p->choose[k][index];
        keys[KEYS_HELD - k] = keycodes[index];
        index--;
    }
}

static bool end_packet(Petit4Send* p, Petit4Packet* packet) {
    if(p->end_step >= sizeof(end_mods)) return false;
    memset(packet, 0, sizeof(Petit4Packet));
    packet->mods = end_mods[p->end_step++];
    return true;
}

static void set_mouse(Petit4Send* p, uint8_t bits) {
    /* SmileBASIC 4 has buttons 4 and 5 the other way round from USB. */
    p->mouse = (bits & 0x07) | ((bits & 0x08) << 1) | ((bits & 0x10) >> 1);
}

static void make_packet(Petit4Send* p, uint64_t value, Petit4Packet* packet) {
    choose_keys(p, value & MASK_36, packet->keys);

    /* The receiver checks for a new packet by these two bits changing. */
    packet->mods = 0;
    if(p->clock & 1) packet->mods |= HID_MOD_GUI;
    if(p->clock & 2) packet->mods |= HID_MOD_SHIFT;
    if(value >> 36) packet->mods |= HID_MOD_ALT;
    packet->mouse = p->mouse;
    p->clock++;
}

static void fill_lead(Petit4Send* p, int want) {
    while(p->bit_count < want) {
        uint32_t at = p->lead_pos++;
        uint64_t byte = at >= 16 && at < 16 + 50 ? 1 : at == HEADER_LEADER ? p->parity : 0;
        p->bits |= byte << p->bit_count;
        p->bit_count += 8;
    }
}

static bool parity_next_packet(Petit4Send* p, Petit4Packet* packet) {
    uint64_t value;
    if(p->clock < PARITY_LEAD_PACKETS) {
        fill_lead(p, PACKET_BITS);
        value = take_bits(p, PACKET_BITS);
        if((p->clock & 1) == 0) {
            fill_lead(p, MOUSE_BITS);
            set_mouse(p, take_bits(p, MOUSE_BITS));
        }
        make_packet(p, value, packet);
        if(p->clock == PARITY_LEAD_PACKETS) { /* the rest of the lead was zero bits */
            p->bits = 0;
            p->bit_count = 0;
        }
        return true;
    }

    if(p->group_pos == p->parity - 1) {
        make_packet(p, p->xor_value, packet);
        p->group_pos = 0;
        p->xor_value = 0;
        p->xor_mouse = 0;
        return true;
    }
    fill_bits(p, PACKET_BITS);
    if(p->group_pos == 0 && p->bit_count == 0) return end_packet(p, packet);

    value = take_bits(p, PACKET_BITS); /* the last group is padded with zero bits */
    p->xor_value ^= value;
    if((p->group_pos & 1) == 0) {
        /* The parity packet's mouse bits go out with the even packet before it, and
         * only an odd packet's are data: an even one counts as having none. */
        if(p->group_pos + 2 == p->parity) {
            set_mouse(p, p->xor_mouse);
        } else {
            fill_bits(p, MOUSE_BITS);
            uint8_t bits = take_bits(p, MOUSE_BITS);
            p->xor_mouse ^= bits;
            set_mouse(p, bits);
        }
    }
    p->group_pos++;
    make_packet(p, value, packet);
    return true;
}

void petit4_set_multi(Petit4Send* p, uint8_t keyboards) {
    p->multi = keyboards;
    if(keyboards && !p->parity) p->parity = PETIT4_MULTI_PARITY;
}

/* A keyboard's keys always differ from the packet before: a report not yet come shows as
 * the same keys. Values are first XORed with a mask for their place, so that a run of one
 * value, as zero padding is, does not repeat; one that still equals the code just sent goes
 * as MULTI_SAME, which means the masked value before. Lead packets repeat freely and are
 * not compared. */
static uint32_t multi_mask(uint32_t place, uint32_t board) {
    return ((place + 1) * 40503UL + (board + 1) * 25931UL) & (MULTI_SAME - 1);
}

static void multi_make(Petit4Send* p, const uint32_t* values, Petit4MultiPacket* packet, int place) {
    memset(packet->keys, 0, sizeof(packet->keys));
    for(int board = 0; board < p->multi; board++) {
        const uint8_t* keys = &p->multi_keys[board * MULTI_KEYS_EACH];
        uint32_t value = values[board];
        if(place >= 0) {
            value ^= multi_mask(place, board);
            if(value == p->multi_last[board]) value = MULTI_SAME;
        }
        p->multi_last[board] = value;
        int index = MULTI_KEYS_EACH - 1;
        for(int k = KEYS_HELD; k >= 1; k--) {
            while(p->choose[k][index] > value) {
                index--;
            }
            value -= p->choose[k][index];
            packet->keys[board][KEYS_HELD - k] = keys[index];
            index--;
        }
    }
    packet->mods = 0;
    if(p->clock & 1) packet->mods |= HID_MOD_GUI;
    if(p->clock & 2) packet->mods |= HID_MOD_SHIFT;
    p->clock++;
}

bool petit4_next_multi(Petit4Send* p, Petit4MultiPacket* packet) {
    uint32_t values[PETIT4_MULTI_KEYBOARDS] = {0};
    if(p->clock < PETIT4_MULTI_LEAD) {
        values[0] = MULTI_MARK;
        values[1] = p->parity;
        values[2] = (PETIT4_MULTI_LEAD - 1 - p->clock) | ((uint32_t)p->multi << 8);
        multi_make(p, values, packet, -1);
        return true;
    }
    if(p->group_pos == p->parity - 1) {
        multi_make(p, p->multi_xor, packet, p->group_pos);
        p->group_pos = 0;
        memset(p->multi_xor, 0, sizeof(p->multi_xor));
        return true;
    }
    fill_bits(p, MULTI_VALUE_BITS);
    if(p->group_pos == 0 && p->bit_count == 0) {
        if(p->end_step >= sizeof(end_mods)) return false;
        memset(packet, 0, sizeof(Petit4MultiPacket));
        packet->mods = end_mods[p->end_step++];
        return true;
    }
    for(int board = 0; board < p->multi; board++) {
        fill_bits(p, MULTI_VALUE_BITS);
        values[board] = take_bits(p, MULTI_VALUE_BITS); /* the last group is padded with zero bits */
        p->multi_xor[board] ^= values[board];
    }
    multi_make(p, values, packet, p->group_pos);
    p->group_pos++;
    return true;
}

void petit4_multi_from_packet(const Petit4Packet* from, Petit4MultiPacket* to) {
    memset(to, 0, sizeof(Petit4MultiPacket));
    to->mods = from->mods;
    memcpy(to->keys[0], from->keys, sizeof(from->keys));
}

/* As SB4 Probe's mode J, the form a real Switch has been seen to read. */
static const uint8_t multi_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, /* Generic Desktop, Keyboard */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, /* modifiers */
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, /* reserved */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, /* keys */
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,
    0xC0,
};

const uint8_t* petit4_multi_report_desc(uint16_t* size) {
    *size = sizeof(multi_report_desc);
    return multi_report_desc;
}

void petit4_multi_config_desc(uint8_t* out, uint8_t keyboards) {
    const uint8_t config[9] = {
        9, 2, (uint8_t)(9 + keyboards * 25), 0, keyboards, 1, 0, 0xC0, 50};
    memcpy(out, config, sizeof(config));
    out += sizeof(config);
    for(uint8_t board = 0; board < keyboards; board++) {
        const uint8_t bytes[25] = {
            9, 4, board, 0, 1, 3, 1, 1, 0,
            9, 0x21, 0x10, 0x01, 0, 1, 0x22, sizeof(multi_report_desc), 0,
            7, 5, (uint8_t)(0x81 + board), 3, 8, 0, 2,
        };
        memcpy(out, bytes, sizeof(bytes));
        out += sizeof(bytes);
    }
}

bool petit4_next_packet(Petit4Send* p, Petit4Packet* packet) {
    if(p->parity) return parity_next_packet(p, packet);

    fill_bits(p, PACKET_BITS);
    if(p->bit_count == 0) return end_packet(p, packet);

    uint64_t value = take_bits(p, PACKET_BITS);

    /* Mouse bits follow an even packet in the stream and stay for the next
     * one, which is the packet the receiver reads them with. */
    if((p->clock & 1) == 0) {
        fill_bits(p, MOUSE_BITS);
        set_mouse(p, take_bits(p, MOUSE_BITS));
    }
    make_packet(p, value, packet);
    return true;
}

size_t petit4_position(const Petit4Send* p) {
    return p->position;
}

size_t petit4_total(const Petit4Send* p) {
    if(p->parity) return HEADER_SIZE - HEADER_LEADER + p->transfer_size + PARITY_TAIL_SIZE;
    return HEADER_SIZE + p->transfer_size + TAIL_SIZE;
}

const char* petit4_name(const Petit4Send* p) {
    return p->name;
}

const char* petit4_error(const Petit4Send* p) {
    return p->error;
}

#define PART_PREFIX_SIZE 12

bool petit4_batch_is_p4a(const uint8_t* head, size_t len) {
    return len >= 4 && (memcmp(head, "P4A4", 4) == 0 || memcmp(head, "P4A2", 4) == 0 ||
                        memcmp(head, "P4A1", 4) == 0);
}

const char* petit4_batch_parse(
    Petit4Batch* batch,
    const uint8_t* head,
    size_t len,
    uint64_t file_size,
    uint32_t start_part) {
    memset(batch, 0, sizeof(Petit4Batch));
    if(len >= 4 && (memcmp(head, "P4A1", 4) == 0 || memcmp(head, "P4A2", 4) == 0)) {
        return "Old P4A: use new p4pack";
    }
    if(len < 4 || memcmp(head, "P4A4", 4) != 0) return "Not a P4A file";
    if(len < PETIT4_BATCH_HEAD_SIZE || file_size < PETIT4_BATCH_HEAD_SIZE) {
        return "P4A file too short";
    }
    if(file_size > 0x7FFFFFFFU) return "P4A file too large";
    uint32_t part_size = read_u32(head + 8);
    uint32_t count = read_u32(head + 12);
    uint32_t image_size = read_u32(head + 16);
    uint32_t parity = read_u32(head + 20);
    if(parity != 0 && (parity < 4 || parity > 64 || (parity & 1) != 0)) {
        return "P4A parity is bad";
    }
    if(part_size == 0 || (part_size & 3) != 0) return "P4A part size is bad";
    if(image_size == 0 || image_size > 0x7FFFFFFFU) return "P4A image size is bad";
    if(count != (image_size - 1) / part_size + 1) return "P4A part count is bad";
    if(count > PETIT4_BATCH_MAX_PARTS) return "P4A has too many parts";
    if(file_size < PETIT4_BATCH_HEAD_SIZE + (uint64_t)count * PETIT4_BATCH_ROW_SIZE) {
        return "P4A file too short";
    }
    batch->id = read_u32(head + 4);
    batch->part_size = part_size;
    batch->part_count = count;
    batch->image_size = image_size;
    batch->parity = parity;
    batch->file_size = file_size;
    if(start_part >= count) return "Start Part past the end";
    return NULL;
}

const char* petit4_part_row_parse(
    Petit4PartRow* row,
    const Petit4Batch* batch,
    uint32_t part,
    const uint8_t* raw) {
    row->offset = read_u32(raw);
    row->sent = read_u32(raw + 4);
    row->original = read_u32(raw + 8);
    row->crc = raw[12] | (raw[13] << 8);
    row->comp = raw[14];

    uint32_t table_end = PETIT4_BATCH_HEAD_SIZE + batch->part_count * PETIT4_BATCH_ROW_SIZE;
    if(row->offset < table_end || row->offset > batch->file_size ||
       row->sent > batch->file_size - row->offset) {
        return "P4A row: past file end";
    }
    uint32_t piece = batch->image_size - part * batch->part_size;
    if(piece > batch->part_size) piece = batch->part_size;
    /* The prefix and the piece, padded to a multiple of 4. */
    uint32_t original = (PART_PREFIX_SIZE + piece + 3) & ~3UL;
    if(row->original != original) return "P4A row: bad length";
    if(row->sent == 0 || row->sent > row->original) return "P4A row: bad sent length";
    if(row->comp > 2 || raw[15] != 0) return "P4A row: bad compression";
    if(row->comp == 0 && row->sent != row->original) return "P4A row: bad sent length";
    return NULL;
}

static bool part_row_read(
    Petit4Send* p,
    const Petit4Batch* batch,
    uint32_t part,
    Petit4PartRow* row) {
    uint8_t raw[PETIT4_BATCH_ROW_SIZE];
    p->error = "Cannot read file";
    p->file_pos = 0;
    p->file_len = 0;
    uint32_t at = PETIT4_BATCH_HEAD_SIZE + part * PETIT4_BATCH_ROW_SIZE;
    if(!p->seek(p->context, at)) return false;
    if(p->read(p->context, raw, sizeof(raw)) != sizeof(raw)) return false;
    const char* error = petit4_part_row_parse(row, batch, part, raw);
    if(error) {
        p->error = error;
        return false;
    }
    return true;
}

bool petit4_part_begin(
    Petit4Send* p,
    const Petit4Batch* batch,
    uint32_t part,
    Petit4PartRow* row) {
    if(!part_row_read(p, batch, part, row)) return false;
    p->is_window = true;
    p->is_image = false;
    p->is_text = false;
    p->compress = false; /* whatever it is, the PC has done it */
    p->parity = batch->parity;
    if(p->multi && !p->parity) p->parity = PETIT4_MULTI_PARITY;
    p->width = 0;
    p->height = 0;
    p->file_size = row->original;
    p->transfer_size = row->sent;
    p->crc = row->crc;
    p->window_offset = row->offset;
    p->window_size = row->sent;

    memcpy(p->name, "DAT:P4A", 7);
    uint32_t digits = part;
    for(int i = 4; i >= 0; i--) {
        p->name[7 + i] = '0' + digits % 10;
        digits /= 10;
    }
    p->name[12] = '\0';

    build_stream_ends(p, row->comp);
    return petit4_restart(p);
}

void petit4_indent_init(Petit4Indent* in) {
    memset(in, 0, sizeof(Petit4Indent));
    in->at_start = true;
}

static void indent_settle(Petit4Indent* in) {
    if(in->indent > in->lead) in->backspaces = in->indent - in->lead;
    if(in->lead > in->indent) in->spaces = in->lead - in->indent;
    in->indent = in->lead;
    in->at_start = false;
}

bool petit4_indent_byte(Petit4Indent* in, uint8_t c) {
    if(c == '\r') return false; // A keyboard cannot type it, and CRLF is one newline
    if(in->at_start) {
        if(c == ' ' || c == '\t') {
            in->lead += c == ' ' ? 1 : PETIT4_TAB_STEP;
            return false;
        }
        indent_settle(in);
    }
    if(c == '\n') {
        in->lead = 0;
        in->at_start = true;
    }
    return true;
}

void petit4_indent_end(Petit4Indent* in) {
    if(in->at_start) indent_settle(in);
}

Petit4IndentKey petit4_indent_key(Petit4Indent* in) {
    if(in->backspaces) {
        in->backspaces--;
        return Petit4IndentBackspace;
    }
    if(in->spaces) {
        in->spaces--;
        return Petit4IndentSpace;
    }
    return Petit4IndentNone;
}

void petit4_tap_start(Petit4Tap* tap, uint32_t now, uint32_t wait_end, bool forever) {
    tap->last_press = now;
    tap->quiet_from = wait_end - PETIT4_TAP_QUIET_MS;
    tap->forever = forever;
    tap->down = false;
}

Petit4TapAction petit4_tap_next(const Petit4Tap* tap, uint32_t now) {
    int32_t since = (int32_t)(now - tap->last_press);
    if(tap->down) return since >= PETIT4_TAP_HOLD_MS ? Petit4TapRelease : Petit4TapNone;
    if(since < PETIT4_TAP_SPACING_MS) return Petit4TapNone;
    /* With room for the release to be late and still before the quiet part. */
    if(!tap->forever && (int32_t)(now + PETIT4_TAP_HOLD_MS + 100 - tap->quiet_from) > 0) {
        return Petit4TapNone;
    }
    return Petit4TapPress;
}

void petit4_tap_done(Petit4Tap* tap, Petit4TapAction action, uint32_t now) {
    if(action == Petit4TapPress) {
        tap->down = true;
        tap->last_press = now;
    } else if(action == Petit4TapRelease) {
        tap->down = false;
    }
}

void petit4_tap_packet(Petit4TapAction action, Petit4Packet* packet) {
    memset(packet, 0, sizeof(Petit4Packet));
    if(action == Petit4TapPress) packet->keys[0] = 0x28;
}

void petit4_run_init(
    Petit4Run* run,
    const Petit4Batch* batch,
    uint32_t start_part,
    uint32_t wait_ms) {
    memset(run, 0, sizeof(Petit4Run));
    run->batch = *batch;
    run->start_part = start_part;
    run->part = start_part;
    run->end_part = batch->part_count - 1;
    run->wait_ms = wait_ms;
    run->stage = Petit4RunStageWait;
}

void petit4_run_set_multi(Petit4Run* run, uint8_t keyboards) {
    run->multi = keyboards;
    if(keyboards && !run->batch.parity) run->batch.parity = PETIT4_MULTI_PARITY;
}

static bool run_begin_part(Petit4Run* run, Petit4Send* p) {
    Petit4PartRow row;
    if(!petit4_part_begin(p, &run->batch, run->part, &row)) return false;
    run->sent_cur = row.sent;
    return true;
}

bool petit4_run_open(Petit4Run* run, Petit4Send* p) {
    Petit4PartRow row;
    uint32_t total = 0;
    for(uint32_t part = 0; part < run->batch.part_count; part++) {
        if(!part_row_read(p, &run->batch, part, &row)) return false;
        if(part >= run->start_part) total += row.sent;
    }
    run->sent_from_start = total;
    run->sent_left = total;
    return run_begin_part(run, p);
}

/* A wait that is over at once and has no taps in it. */
static void run_wait_none(Petit4Run* run, uint32_t now) {
    run->stage = Petit4RunStageWait;
    run->wait_end = now;
    petit4_tap_start(&run->tap, now, now, false);
}

void petit4_run_restart(Petit4Run* run, uint32_t now) {
    run->part = run->start_part;
    run->end_part = run->batch.part_count - 1;
    run->sent_left = run->sent_from_start;
    run_wait_none(run, now);
}

bool petit4_run_restart_one(Petit4Run* run, Petit4Send* p, uint32_t part, uint32_t now) {
    Petit4PartRow row;
    if(part >= run->batch.part_count) {
        p->error = "Part past the end";
        return false;
    }
    if(!part_row_read(p, &run->batch, part, &row)) return false;
    run->part = part;
    run->end_part = part;
    run->sent_left = row.sent;
    run_wait_none(run, now);
    return true;
}

void petit4_run_part_sent(Petit4Run* run, uint32_t now) {
    run->sent_left -= run->sent_cur;
    if(run->part >= run->end_part) {
        run->stage = Petit4RunStageConfirm;
        petit4_tap_start(&run->tap, now, now, true);
        return;
    }
    run->part++;
    run->stage = Petit4RunStageWait;
    run->wait_end = now + run->wait_ms;
    petit4_tap_start(&run->tap, now, run->wait_end, false);
}

Petit4RunEvent
    petit4_run_step(Petit4Run* run, Petit4Send* p, uint32_t now, Petit4Packet* packet, int32_t* wait) {
    *wait = 0;
    Petit4TapAction action = petit4_tap_next(&run->tap, now);
    if(action != Petit4TapNone) {
        petit4_tap_packet(action, packet);
        return action == Petit4TapPress ? Petit4RunEventTapPress : Petit4RunEventTapRelease;
    }

    if(run->stage == Petit4RunStageWait && !run->tap.down &&
       (int32_t)(now - run->wait_end) >= 0) {
        if(!run_begin_part(run, p)) return Petit4RunEventError;
        run->stage = Petit4RunStageSend;
        return Petit4RunEventStartStream;
    }

    /* Awake often enough for Stop, and for the times below. */
    int32_t since = (int32_t)(now - run->tap.last_press);
    int32_t until = run->tap.down ? PETIT4_TAP_HOLD_MS - since : PETIT4_TAP_SPACING_MS - since;
    if(run->stage == Petit4RunStageWait && !run->tap.down) {
        int32_t to_end = (int32_t)(run->wait_end - now);
        if(to_end < until) until = to_end;
    }
    if(until > 50) until = 50;
    *wait = until > 1 ? until : 1;
    return Petit4RunEventIdle;
}

void petit4_run_shift(Petit4Run* run, uint32_t ms) {
    run->wait_end += ms;
    run->tap.quiet_from += ms;
}

uint32_t petit4_run_wait_left_s(const Petit4Run* run, uint32_t now) {
    int32_t left = (int32_t)(run->wait_end - now);
    return left > 0 ? ((uint32_t)left + 999) / 1000 : 0;
}

uint32_t petit4_remaining_s(uint32_t left, uint32_t done, uint32_t send_ms, uint32_t fixed_rate) {
    if(send_ms < 5000 || done == 0) return fixed_rate ? left / fixed_rate : 0;
    return (uint64_t)left * send_ms / done / 1000;
}

uint32_t petit4_part_packets(uint32_t parity, uint32_t sent) {
    uint64_t bits = (uint64_t)(HEADER_SIZE - HEADER_LEADER + sent + PARITY_TAIL_SIZE) * 8;
    uint32_t group_bits = parity / 2 * (2 * PACKET_BITS + MOUSE_BITS) - (PACKET_BITS + MOUSE_BITS);
    uint32_t groups = (bits + group_bits - 1) / group_bits;
    return PARITY_LEAD_PACKETS + groups * parity + sizeof(end_mods);
}

uint32_t petit4_part_packets_multi(uint8_t keyboards, uint32_t parity, uint32_t sent) {
    uint64_t bits = (uint64_t)(HEADER_SIZE - HEADER_LEADER + sent + PARITY_TAIL_SIZE) * 8;
    uint32_t group_bits = (parity - 1) * keyboards * MULTI_VALUE_BITS;
    uint32_t groups = (bits + group_bits - 1) / group_bits;
    return PETIT4_MULTI_LEAD + groups * parity + sizeof(end_mods);
}

/* All packets of the parts from this one to the run's last, each stored as prefix, piece
 * and padding. */
static uint64_t parity_parts_packets(const Petit4Run* run, uint32_t from) {
    const Petit4Batch* batch = &run->batch;
    uint32_t count = batch->part_count;
    uint32_t end = run->end_part;
    if(from > end) return 0;
    uint32_t piece = batch->image_size - (count - 1) * batch->part_size;
    uint32_t last_sent = (PART_PREFIX_SIZE + piece + 3) & ~3UL;
    uint32_t each_sent = PART_PREFIX_SIZE + batch->part_size;
    uint32_t last = run->multi ? petit4_part_packets_multi(run->multi, batch->parity, last_sent) :
                                 petit4_part_packets(batch->parity, last_sent);
    uint32_t each = run->multi ? petit4_part_packets_multi(run->multi, batch->parity, each_sent) :
                                 petit4_part_packets(batch->parity, each_sent);
    if(end + 1 < count) return (uint64_t)(end - from + 1) * each;
    return last + (uint64_t)(end - from) * each;
}

uint32_t petit4_run_remaining_s(
    const Petit4Run* run,
    const Petit4Send* p,
    uint32_t now,
    uint32_t packet_us) {
    if(run->stage == Petit4RunStageConfirm) return PETIT4_REMAIN_NONE;

    uint32_t parts_after = run->end_part - run->part;
    uint32_t waits_s = parts_after * (run->wait_ms / 1000);
    if(run->batch.parity) {
        uint64_t packets;
        if(run->stage == Petit4RunStageSend) {
            uint32_t all = run->multi ?
                               petit4_part_packets_multi(run->multi, run->batch.parity, p->transfer_size) :
                               petit4_part_packets(run->batch.parity, p->transfer_size);
            uint32_t done = p->clock + p->end_step;
            packets = parity_parts_packets(run, run->part + 1) +
                      (all > done ? all - done : 0);
        } else {
            packets = parity_parts_packets(run, run->part);
            waits_s += petit4_run_wait_left_s(run, now);
        }
        return packets * packet_us / 1000000 + waits_s;
    }
    uint64_t bytes = (uint64_t)parts_after * (HEADER_SIZE + TAIL_SIZE);
    if(run->stage == Petit4RunStageSend) {
        size_t total = petit4_total(p);
        size_t position = petit4_position(p);
        if(position > total) position = total;
        bytes += (total - position) + (run->sent_left - run->sent_cur);
    } else {
        bytes += HEADER_SIZE + TAIL_SIZE + run->sent_left;
        waits_s += petit4_run_wait_left_s(run, now);
    }
    /* 79 bits in two packets, and the packets that end each stream. */
    uint64_t packets = bytes * 16 / (2 * PACKET_BITS + MOUSE_BITS) +
                       (uint64_t)(parts_after + 1) * sizeof(end_mods);
    return packets * packet_us / 1000000 + waits_s;
}

uint32_t petit4_slow_rate(uint32_t interval_ms) {
    /* 39.5 bits = 4.9375 bytes a packet */
    return interval_ms ? 4938 / interval_ms : 0;
}

static bool fast_is_due(uint32_t step, uint8_t sync) {
    uint32_t distance = (step % PETIT4_SYNC_KEYS + PETIT4_SYNC_KEYS - sync) % PETIT4_SYNC_KEYS;
    return distance & 1;
}

uint32_t petit4_fast_next(uint32_t ms, uint8_t sync) {
    uint32_t step = ms / FAST_STEP_MS + 1;
    while(!fast_is_due(step, sync)) {
        step++;
    }
    return step * FAST_STEP_MS;
}

uint32_t petit4_fast_after_send(uint32_t ms, uint8_t sync) {
    uint32_t next = petit4_fast_next(ms, sync);
    while(next - ms < FAST_MIN_SPACING_MS) {
        next = petit4_fast_next(next, sync);
    }
    return next;
}

uint8_t petit4_detect_key(uint32_t ms) {
    return 4 + (ms / FAST_STEP_MS) % PETIT4_SYNC_KEYS;
}
