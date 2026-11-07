# Protocol

How the parts of p4sendx-tools talk to each other. To use them, read the [guide](guide.md).

- [Protocol](#protocol)
  - [Overview](#overview)
  - [Packets](#packets)
    - [Timing](#timing)
  - [Stream](#stream)
  - [Parity](#parity)
  - [NS links](#ns-links)
  - [Archive](#archive)
    - [Image](#image)
    - [Stream of an archive](#stream-of-an-archive)
    - [File on the SD card](#file-on-the-sd-card)
    - [Parts](#parts)
  - [Sender](#sender)
  - [Receiver](#receiver)
  - [Signal](#signal)
  - [Sounds](#sounds)
  - [Seen on hardware](#seen-on-hardware)

## Overview

The link runs one way: the sender presents a USB keyboard, and SmileBASIC 4 (SB4) reads key states
once a frame. The sender never learns what SB4 does, so a wrong guess costs time or a resend, and a
CRC keeps a bad file from being written.

Three layers carry a file:

| Layer | Carries | Sections |
|---|---|---|
| Packet | Bits, as keys and buttons held at one moment | [Packets](#packets), [NS links](#ns-links) |
| Stream | One file: header, data, CRC | [Stream](#stream), [Parity](#parity) |
| Archive | Many files, as a compressed image cut into parts, each part one stream | [Archive](#archive) |

All numbers are little endian. Bits go lowest first.

## Packets

This is the `Classic` link, the format of Petit4Send. A keyboard report and a 5-button mouse report
go together.

An even packet carries 37 bits. An odd packet carries 5 mouse bits, then 37 bits.

**Keys.** A table `KEYCODE%` in `P4SENDX.PRG` lists 195 USB usages. A packet holds exactly 6 of them.
With their indexes `k0 < k1 < ... < k5`, the low 36 bits are:

    C(k0, 1) + C(k1, 2) + C(k2, 3) + C(k3, 4) + C(k4, 5) + C(k5, 6)

Alt held is bit 36.

**Clock.** Win is clock bit 0 and Shift is clock bit 1, counting packets from 0. A skipped value shows a
lost packet. Four lost in a row do not show.

**Mouse.** Bits 3 and 4 swap between the stream and the USB buttons. SB4 reads them from `MBUTTON()`.
The sender sets them with an even packet, and the receiver reads them with the next odd one.

**End.** After a stream come 4 packets with no keys and modifier values 10, 2, 8, 0.

**A wrong key count** means a torn packet, which counts as lost.

### Timing

- **Slow:** one packet every 24 ms, or `P4S Interval`. SB4 sees each packet in at least one frame.
- **Fast:** one packet per frame, needing a sync key. Steps of 8 ms, counted over 25 values, make 200 ms,
  or 12 frames. A packet goes when `(step - key) mod 25` is odd, and never less than 12 ms after the one
  before.
- **Detect:** the sender holds usage `4 + step mod 25`, changing every step. The receiver reads which
  steps its frames fall on, and works out the key.

The steps come from the USB frame number, so a key lasts while USB stays connected.

## Stream

    size
    16   zeros
    50   ones (bytes of value 1)
    1    zero
    36   type and name, such as "TXT:HELLO", zero padded
    4    file length
    2    image width, for GRP:
    2    image height, for GRP:
    1    compression: 0 none, 1 LZ
    4    transfer length
    ...  data
    2    CRC-16 of the file's bytes: polynomial 0xC7ED, init and final XOR 0xFFFF
    16   zeros

Compression 1 is P4SEND's LZ: a 0 bit and a byte, or a 1 bit, distance - 1 in 10 bits and length - 1 in
5 bits.

| Type | Bytes |
|---|---|
| `TXT:` | UTF-16LE |
| `DAT:` | 32-bit integers |
| `GRP:` | BGRA pixels |

Name part: 1 to 32 characters of `A-Z 0-9 - @ _ .`, not starting with `.`. SB4 takes a whole file name:
`LOAD "MAIN"` does not find `MAIN.PRG`. So senders drop only `.txt .dat .bas .bmp .png .jpg .jpeg`.

## Parity

Parity repairs a lost or torn packet when it is the only one in its group. Added on P4SENDX, and not compatible with original PETIT4SEND.

    packets 0 to 13   leader: 16 zeros, 50 ones, a zero, one byte G, then zero bits up to bit 553
    from packet 14    groups of G packets, starting at an even packet
    after the groups  the 4 end packets

G would be 16, 32 or 64. A name starts with a letter, so a byte under 65 after the leader means parity.

Each packet is a unit of 42 bits: 5 mouse bits and 37 key bits, the mouse bits counting as zero in an
even packet. The first G - 1 packets carry data in stream order. Packet G - 1 is odd and carries the XOR
of those units. Its mouse bits go with the even packet before it, so that even packet carries no data in
them. A group holds `G/2 * 79 - 42` bits: 1222 bits, about 153 bytes, for G 32.

The data is everything from the name to the CRC, without leader or trailing zeros. Zero bits fill the
last group. A stream of n data bytes takes:

    14 + G * ceil((49 + n + 2) * 8 / bits in a group) + 4 packets

At a group's end, the receiver acts on what it counted:

| Missing | Action |
|---|---|
| 0, XOR zero | Accept |
| 0, XOR not zero | Count as misread. Keep the data, and let the CRC decide |
| 1 | The XOR is the missing packet. Put it in place |
| 2 or more | Fail the part or file |

A lost leader packet is the one case parity leaves. Packets 0 to 10 cost nothing, since the receiver
aligns at a later packet. Losing packets 11 to 13 loses the stream. The next part then asks for this one.

## NS links

`NS1` presents 6 boot keyboards and `NS2` 7, with no mouse, on endpoints from 0x81.
Only P4SENDX reads them. They always use parity: off counts as G 32.

**Keys.** Sort the 195 usages ascending and leave out 40 Enter, 41 Esc, 42 Backspace, 43 Tab, 44 Space and
88 keypad Enter. That leaves 189, so each of 7 keyboards gets 27 keys, in order. `NS1` uses the
first 6.

**Values.** A keyboard holds 6 of its 27 keys, which gives a combination number as in [Packets](#packets).
C(27,6) is 296010.

| Code | Meaning |
|---|---|
| 0 to 2^18 - 1 | An 18-bit masked value |
| 2^18 | The masked value this keyboard had before |
| Above | Misread |

A packet is 6 or 7 values, keyboard 1 first: 108 or 126 bits. Win and Shift on keyboard 1 hold the clock.
Only changed reports go, keyboard 1 last. The host polls each endpoint when it likes, so that order is
what is written, and keys may still arrive after the clock.

**Masks.** A keyboard never shows the same keys in two packets running, so unchanged keys mean its report
has not come yet. Each data and parity value is XORed with a mask for its place p in the group and
keyboard k, both from 0:

    ((p + 1) * 40503 + (k + 1) * 25931) mod 2^18

A masked value still equal to the code just sent on that keyboard goes as 2^18. Parity is over the values
before masking. Lead packets are neither masked nor compared.

**Stream.**

    4 lead packets    values: 0x2B4A5, G, lead packets still to come + 256 * keyboards, zeros
    groups            G - 1 packets of data, then a packet of the XOR of each value
    4 end packets     as in Classic

The data is the name to the CRC, value 0 of keyboard 1 first, with zero bits filling the last group. Any
lead packet alone starts a stream. The clock starts at 0.

**Late reports.** A packet is not whole if a keyboard holds other than 6 keys, or the same keys as in the
packet before. The receiver reads it again each frame while the clock stays.

| What happens | Result |
|---|---|
| It becomes whole | Taken, counted as `LATE` |
| The next clock comes first | Missing |
| A keyboard that was right changes | Missing |
| The clock jumps, or a number is misread | Missing |

After a missing packet, the receiver skips this check for the next packet, since it does not know the code
before it. A late report there is misread, and the CRC decides. A `2^18` right after a missing packet takes
the value before that one.

## Archive

### Image

What the files become, before compression:

    4    entry count
    ...  entries:
           36   type and name, zero padded
           4    data size
           2    CRC-16 of the data
           2    zero
           ...  data, zero padded to a multiple of 4

For `GRP:`, the data starts with width and height, 16 bits each.

### Stream of an archive

The PC compresses the whole image, then splits the result. Its length is a multiple of 4:

    4    magic "P4A3"
    4    archive id: CRC-32 (zlib) from offset 8 to the end
    4    method: 0 stored, 1 LZ
    4    image size
    ...  payload, zero padded to a multiple of 4

Method 1 takes flag bits from a flag byte, read from the payload when a flag is needed and none is left:

    0     literal: 1 byte
    1 0   short match: 2 bytes, low 10 bits distance - 1, high 6 bits length - 2
    1 1   long match: 2 bytes distance - 1, then 1 byte length - 3

Distance reaches 1024 or 65536, and length 65 or 258. Matches may overlap, and may cross entries and parts.
It stops at image size, with no end marker. A reader needs a ring of 65536 integers.

### File on the SD card

    4    magic "P4A4"
    4    archive id
    4    part size: 16384 by default, a multiple of 4, at least 16
    4    part count: ceil(stream size / part size)
    4    stream size
    4    G: 16, 32, 64, or 0 for no parity
    16 * part count, one row each:
           4    offset in this file
           4    sent length
           4    original length, the same
           2    CRC-16 of the part
           1    compression: 0
           1    zero
    ...  each part in turn

### Parts

A part is one stream:

- Name: `DAT:P4A` and 5 digits of the part index, from 00000.
- Data: 12 bytes (archive id, part index, part count), then a piece of the archive's stream.
- File and transfer length, and CRC, from its row.

## Sender

The first 4 bytes of a file tell an archive. `P4A4` goes as parts. `P4A1` and `P4A2` get `Old P4A: use new
p4pack`.

From `P4S Start Part`, for each part:

1. Send the stream.
2. For `P4S Part Wait` seconds, tap Enter every 500 ms, held 50 ms, with nothing else held. Stop 1 s before
   the wait ends.

After the last part, keep tapping until the user presses Stop, as SB4 is still writing.

`Send Part` sends one chosen part the same way, then keeps tapping until Stop.

Classic data packets hold A, B and Esc, which would answer a dialog, so no data goes during step 2. With a
wait too short, SB4 sees a cancelled save or damaged data, and asks for the part again.

The Flipper sends a single file with `P4S Parity`, as in [Parity](#parity).

## Receiver

**Telling streams apart.** `USB RECEIVE` takes all of these with one screen:

| Seen | Meaning |
|---|---|
| More than 6 keys | An NS link |
| At least 4 ones, then a zero | A stream. The next byte tells parity |
| A name `DAT:P4Annnnn` | A part |
| Any other name | One file, saved parts left alone |

A file of such a name from a stock sender gets a message about its header or prefix, with no save.

**Parts.** Coming in, P4SENDX lists `P4A*` DAT files. If there are any, it reads them, checks their prefixes
and works out which are missing. It offers the write if all are saved, and a list if there are several
archives or unreadable files.

Any part of the archive not saved yet is taken, in any order. If the first part saved is not part 0, the
archive id and count come from its prefix. A part already saved is dropped with no prompt. A bad CRC, a
cancelled save or a lost packet marks that part failed, and P4SENDX takes the next one. If a stream's header
is lost, its index is taken as the part after the last one seen. Once the last part has passed, P4SENDX lists
the missing parts, with runs as `a-b`. Every part is saved, the last too.

**Writing.** With all parts saved, it checks prefixes, magic, CRC-32, method and image size. It then unpacks
entries in order, one at a time, holding one part and the ring. It checks each entry's CRC and writes with
SAVEV, or SAVEG for GRP. It deletes the parts only if every file went well and the output was exactly image
size.

**Same content.** LOADV asks nothing in a program, so P4SENDX compares an entry with a same-named file. If
they match, it counts `IGNORED` and writes nothing. Any doubt, or too little memory, means writing it.

**One file.** With parity, two lost in a group, or 3 s with no packets, shows `TRY AGAIN`, and P4SENDX waits
for the next stream. Without parity, it carries on through lost packets, and the CRC decides.

**Subprogram.** Under `SUBRUN` (`ENVTYPE` type 1), it sets XSUBSCREEN 0,0,1280,720, XSCREEN 400,240,3,1 and an
opaque BACKCOLOR after each ACLS. ENVPROJECT sends files to the main side's current project. ENVFOCUS 1 takes
the focus.

**Screen.** The screen is GRP0 at 400x240, and pictures on their way to a file go through GRP1. Drawing comes
after the receive loop in a frame, every 0.1 s, only what changed, and never in a frame where a parity group
ended. `SCREENSHOT SEND`'s pages take the whole 1280x720.

**Clock.** MILLISEC is read only after a VSYNC, since it did not move properly inside a long loop. `SAVE` counts
SAVEV and SAVEG with their dialogs. `UNPACK` counts the rest of writing.

## Signal

The icon estimates the chance S that the next part arrives. P4SENDX keeps 15 buckets of 10 s spent inside
streams, from a leader to the stream's end, with E packets expected, M missing including repaired, Gr groups,
and Gf groups not repaired. Time between streams neither counts nor ages the buckets. A stream starts a new
window once the old one holds 10 s, and the icon keeps the old reading until the new window holds 10 s too.

    p = M / E
    f = Gf / Gr
    F = min(max(G(G-1)/2 * p * p, f), 1)
    S = (1 - F) ^ ceil(bytes * 8 / bits in a group)     with parity
    S = (1 - p) ^ (bytes * 16 / 79)                     without

Here bytes is what a part's stream holds, or 16447 for one file. S gives 4 bars at 0.95, 3 at 0.8, 2 at 0.5
and 1 at 0.2, and grey below that or for the first 20 s. A fall shows at once, and a rise after 5 s.

With Classic, G 32 and 16 KiB parts, one packet lost per 4096 bytes gives 92.5%, 3 bars. One per 2048 gives 73%,
2 bars. A simulation with random single losses, leader included, gave 92.0% and 71.8% over 400 tries each.

## Sounds

Each side sounds what it knows. Every note on either side comes from C D E G A at A4 = 440 Hz, so an overlap
stays consonant.

| Side | Notes | Meaning |
|---|---|---|
| SB4, MML `@12` | C E G, C above | Done |
| SB4, MML `@12` | C C C, twice | User needed |
| Flipper | Rising | Sent |
| Flipper | Repeated falling | User needed |

## Seen on hardware

- Enter answers a save dialog with its default OK, twice for an overwrite. A dialog opens 0.3 to 0.5 s after a
  stream ends, measured with stock P4SEND and a random file.
- A save's dialogs take about 2.6 to 5 s per file.
- A program cannot prevent auto-sleep.
- In the editor, Enter starts the next line with the last line's indent as spaces. Backspace removes one space
  whatever the tab stop. Enter on an indented blank line keeps its indent.
- With a USB probe at 24 ms, on streams of 625 steps:

  | Console | Keyboards | Result |
  |---|---:|---|
  | Switch 2 | 7 | Every step exact, none late |
  | Switch 1 | 6 | Every step exact, none late |
  | Switch 1 | 7 | 6, 9 and 14 late steps in three streams |

  On a Switch 2, wider reports were not read: an array of 30 keys, an NKRO bitmap, and an NKRO interface beside
  a boot keyboard. The Flipper counted its reports as unread.
