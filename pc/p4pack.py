#!/usr/bin/env python3
"""Pack files or projects into a .p4a archive, to send to SmileBASIC 4 in one batch
through SB4 Send on a Flipper Zero and P4SENDX.PRG.

    p4pack.py -o OUT.p4a [--part-size BYTES] [--parity 16|32|64|off] [--no-compress] INPUT...

INPUT is a file or a project folder. Subfolders of a project are not included.

    .txt .prg .bas          TXT, UTF-8 converted to UTF-16LE, BOM dropped
    .png .jpg .jpeg .bmp    GRP, up to 2048x2048
    anything else           DAT, the bytes as 32-bit integers

SmileBASIC 4's own files in a project folder are recognised by their header and
need no key:

    TNAME     text                    TXT:NAME
    BNAME     1-dimensional integers  DAT:NAME
    GNAME     an image                GRP:NAME

Files that would not arrive unchanged are skipped with a warning. P4SEND receives
only flat integer arrays, which rules out DATs of reals or strings and arrays of 2
or more dimensions. JPG files are skipped because SAVEV cannot write them, META
because only METASAVE can.

SmileBASIC 4 keeps the whole file name, so only .txt .dat .bas .bmp .png .jpg .jpeg
are dropped: main.prg becomes TXT:MAIN.PRG and map.bin becomes DAT:MAP.BIN. Names are
upper-cased, illegal characters become _ with a warning, and a duplicate name is an
error.

The whole archive is compressed here, with an optimal parse for its format, and then
split, so each part is plain bytes that can be resent alone. P4SENDX unpacks once,
after all parts arrive. The Flipper only copies what is sent.

A failed part is sent again whole, so parts are 16384 bytes by default, and a larger
--part-size gets a warning. One lost packet in a part's parity group of 32 is repaired
without sending anything again.

An archive works on either of SB4 Send's links, and the time on each is printed.

Images need Pillow. The format is in docs/protocol.md.
"""
import argparse
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

STREAM_MAGIC = b"P4A3"
STREAM_HEADER = "<4sIII"  # magic, id, method, image size
METHOD_STORED, METHOD_LZ = 0, 1
FILE_MAGIC = b"P4A4"
FILE_HEADER = "<4sIIIII"
ROW = "<IIIHBB"  # offset, sent length, original length, CRC-16, compression, zero
DEFAULT_PART_SIZE = 16384
MAX_PART_SIZE = 16 << 20  # P4SENDX refuses more; such a part takes 15 hours anyway
MAX_PARTS = 100000  # five digits in the part's name
MAX_ARCHIVE = (2 << 30) - 1  # SB4 Send refuses a larger file, SB4 a larger image
MIN_PART_SIZE = 16  # the stream's header stays in the first part
PREFIX_SIZE = 12  # archive id, part index, part count, in front of each part
ENTRY_HEADER_SIZE = 44
NAME_FIELD = 36

SLOW_PACKET_MS = 24  # one packet each
FAST_PACKETS_PER_200_MS = 12  # a sync key's 25 steps of 8 ms leave 12 packets
PART_WAIT = 10  # seconds, the Flipper's default
STREAM_OVERHEAD = 16 + 50 + 1 + 4 + 32 + 4 + 4 + 1 + 4 + 2 + 16  # leader, header, CRC, tail
MULTI_LEAD_PACKETS = 4
MULTI_PACKET_MS = 24
END_PACKETS = 4  # clock values with no keys held, after the stream
PARITY_CHOICES = {"16": 16, "32": 32, "64": 64, "off": 0}  # packets in a group
DEFAULT_PARITY = "32"
PARITY_LEAD_PACKETS = 14  # the leader, G and padding, up to the first group
PARITY_DATA_OVERHEAD = 49 + 2  # name, sizes, compression and length, then the CRC

# Tokens of method 1. Flag bits come lowest first from a flag byte, read from the
# payload when a flag is needed and none is left; all else is whole bytes, so that
# BASIC reads it quickly.
#   0    literal: the byte
#   1 0  short match: 16 bits, distance - 1 in the low 10, length - 2 in the high 6
#   1 1  long match: 16 bits distance - 1, 8 bits length - 3
LITERAL_BITS = 9
SHORT_BITS, SHORT_DISTANCE, SHORT_SHORTEST, SHORT_LONGEST = 18, 1 << 10, 2, 65
LONG_BITS, LONG_DISTANCE, LONG_SHORTEST, LONG_LONGEST = 26, 1 << 16, 3, 258

# SmileBASIC 4's limits.
BODY_LIMIT = 16 << 20  # of a file's body, after deflate if that helps
SLOT_LIMIT = 1 << 20  # characters LOAD takes into a program slot
GRP_MAX_SIDE = 2048
NAME_LIMIT = 32
RESERVED_NAME = re.compile(r"(CON|PRN|AUX|NUL|COM\d|LPT\d)(\..*)?")
PART_NAME = re.compile(r"P4A\d{5}")

TEXT_SUFFIXES = {".txt", ".prg", ".bas"}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp"}
DROPPED_SUFFIXES = {".txt", ".dat", ".bas", ".bmp", ".png", ".jpg", ".jpeg"}

CONTAINER_SIZE = 0x70
SIGNATURE_SIZE = 20
KIND_TEXT, KIND_DATA, KIND_GRP, KIND_META, KIND_JPG = 0, 1, 2, 4, 5
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class Skip(Exception):
    """This file cannot be represented faithfully."""


class Refuse(Exception):
    """The archive cannot be made."""


def crc16(data):
    """P4SEND's CRC: polynomial 0xC7ED, init and final XOR 0xFFFF."""
    table = CRC_TABLE
    crc = 0xFFFF
    for byte in data:
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFF


def crc_table():
    table = []
    for byte in range(256):
        value = byte
        for _ in range(8):
            value = 0xC7ED ^ (value >> 1) if value & 1 else value >> 1
        table.append(value)
    return table


CRC_TABLE = crc_table()


@dataclass
class Entry:
    kind: str  # TXT, DAT or GRP
    name: str
    data: bytes
    source: Path
    notes: tuple = ()

    @property
    def full_name(self):
        return f"{self.kind}:{self.name}"


def text_to_utf16(raw, path):
    """UTF-8 to what a SmileBASIC string holds. It writes each UTF-16 unit on its
    own, so a character past U+FFFF may come from its file as two 3-byte sequences."""
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    try:
        text = raw.decode("utf-8", "surrogatepass")
    except UnicodeDecodeError as error:
        raise Skip(f"not valid UTF-8 at byte {error.start}") from error
    # Surrogates already apart, as SmileBASIC's files hold them, stay as they are.
    data = text.encode("utf-16-le", "surrogatepass")
    notes = []
    if any(ord(c) > 0xFFFF for c in text):
        notes.append("has characters past U+FFFF: SmileBASIC 4 holds each as two units")
    return data, tuple(notes)


def image_to_grp(path):
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError as error:
        raise Refuse("images need Pillow (the Python package)") from error
    try:
        with Image.open(path) as image:
            width, height = image.size
            if width > GRP_MAX_SIDE or height > GRP_MAX_SIDE:
                raise Skip(f"{width}x{height} is over {GRP_MAX_SIDE}x{GRP_MAX_SIDE}")
            pixels = image.convert("RGBA").tobytes("raw", "BGRA")
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError) as error:
        raise Skip(f"not a readable image: {error}") from error
    return struct.pack("<HH", width, height) + pixels


def pcbn_integers(body, want):
    """The elements of a PCBN array, and its sizes, if P4SEND's way can hold them."""
    if len(body) < 0x1C:
        raise Skip("the array's header is cut short")
    version = body[:8]
    element, dimensions = struct.unpack_from("<HH", body, 8)
    if version != b"PCBN0004":
        raise Skip(f"array format {version!r} is not handled")
    if element == 5:
        raise Skip("an array of reals: P4SEND's integer path would change it")
    if element == 6:
        raise Skip("an array of strings: P4SEND's integer path cannot hold it")
    if element != 4:
        raise Skip(f"array element type {element} is not handled")
    if not 1 <= dimensions <= 4:
        raise Skip(f"an array of {dimensions} dimensions")
    if dimensions != want:
        raise Skip(
            f"an array of {dimensions} dimensions: P4SEND's integer path would flatten it"
        )
    sizes = struct.unpack_from(f"<{dimensions}I", body, 0x0C)
    count = 1
    for size in sizes:
        count *= size
    elements = body[0x1C:]
    if len(elements) != count * 4:
        raise Skip(f"the array holds {len(elements)} bytes for {count} integers")
    return elements, sizes


def grp_from_pcbn(body):
    pixels, (width, height) = pcbn_integers(body, 2)
    if not (1 <= width <= GRP_MAX_SIDE and 1 <= height <= GRP_MAX_SIDE):
        raise Skip(f"an image of {width}x{height}")
    return struct.pack("<HH", width, height) + pixels


def parse_container(raw):
    """(kind, usage, body) of SmileBASIC 4's own file, or None if this is not one."""
    if len(raw) < CONTAINER_SIZE:
        return None
    version, kind, compressed, usage, size = struct.unpack_from("<HHHHI", raw)
    if (
        version != 4
        or kind not in (KIND_TEXT, KIND_DATA, KIND_GRP, KIND_META, KIND_JPG)
        or compressed > 1
        or len(raw) - CONTAINER_SIZE - size not in (0, SIGNATURE_SIZE)
    ):
        return None
    body = raw[CONTAINER_SIZE : CONTAINER_SIZE + size]
    if compressed:
        try:
            body = zlib.decompress(body)
        except zlib.error as error:
            raise Skip(f"its compressed body cannot be read: {error}") from error
    return kind, usage, body


def native_entry(path, raw):
    """An Entry from SmileBASIC 4's own file, or None if this is a PC file."""
    file_name = path.name
    prefix = file_name[0].upper()
    parsed = parse_container(raw)
    name = file_name[1:].upper()
    if parsed is not None:
        kind, usage, body = parsed
        if kind == KIND_JPG or prefix == "J":
            raise Skip("a JPG file: SAVEV cannot write one, and SAVEG would compress it again")
        if kind == KIND_META:
            raise Skip("project metadata: only METASAVE can write it")
        if len(file_name) < 2:
            return None
        if kind == KIND_TEXT and prefix == "T":
            data, notes = text_to_utf16(body, path)
            return Entry("TXT", name, data, path, notes)
        if prefix == "G" and (kind == KIND_GRP or (kind == KIND_DATA and usage == 2)):
            return Entry("GRP", name, grp_from_pcbn(body), path)
        if kind == KIND_DATA and prefix == "B":
            return Entry("DAT", name, pcbn_integers(body, 1)[0], path)
        raise Skip(f"its name's first letter {prefix} does not fit its header (kind {kind})")
    # Without the container: bare arrays, and PNG images in SB4's own projects.
    if "." not in file_name and len(file_name) > 1:
        if raw.startswith(b"PCBN") and prefix == "B":
            return Entry("DAT", name, pcbn_integers(raw, 1)[0], path)
        if raw.startswith(b"PCBN") and prefix == "G":
            return Entry("GRP", name, grp_from_pcbn(raw), path)
        if raw.startswith(PNG_SIGNATURE) and prefix == "G":
            return Entry("GRP", name, image_to_grp(path), path)
    if raw.startswith(b"PCBN"):
        return Entry("DAT", file_stem_name(path), pcbn_integers(raw, 1)[0], path)
    return None


def file_stem_name(path):
    """The name of a PC file: upper case, without an extension that only says its format."""
    name = path.name
    suffix = path.suffix.lower()
    if suffix in DROPPED_SUFFIXES and path.stem:
        name = path.stem
    return name.upper()


def legal_name(name, source, warn):
    """A name SmileBASIC 4 takes, by changing what it would refuse."""
    cleaned = re.sub(r"[^A-Z0-9_@.\-]", "_", name)
    if cleaned.startswith("."):
        cleaned = "_" + cleaned[1:]
    if len(cleaned) > NAME_LIMIT:
        cleaned = cleaned[:NAME_LIMIT].rstrip(".") or "_"
    if not cleaned:
        raise Skip("its name has no usable characters")
    if RESERVED_NAME.fullmatch(cleaned):
        cleaned = ("_" + cleaned)[:NAME_LIMIT]
    if cleaned != name:
        warn(f"{source}: named {cleaned}, as SmileBASIC 4 would refuse {name}")
    return cleaned


def read_file(path):
    """An Entry for one file. Raises Skip."""
    raw = path.read_bytes()
    entry = native_entry(path, raw)
    if entry is not None:
        return entry
    suffix = path.suffix.lower()
    name = file_stem_name(path)
    if suffix in IMAGE_SUFFIXES:
        return Entry("GRP", name, image_to_grp(path), path)
    if suffix in TEXT_SUFFIXES:
        data, notes = text_to_utf16(raw, path)
        return Entry("TXT", name, data, path, notes)
    notes = ()
    if len(raw) % 4:
        notes = (f"{len(raw)} bytes: SmileBASIC 4 holds integers, so it ends with zeros",)
    return Entry("DAT", name, raw, path, notes)


def gather(inputs, output, warn):
    """The files to pack, in a fixed order."""
    files = []
    for given in inputs:
        if given.is_dir():
            for path in sorted(given.iterdir()):
                if path.is_dir():
                    warn(f"{path}: skipped, a folder inside a project")
                elif path.name.startswith(".") or path.name in ("Thumbs.db", "desktop.ini"):
                    warn(f"{path}: skipped, a file of the computer's own")
                elif path.name.upper().lstrip("TBGJ").startswith("@BACKUP"):
                    warn(f"{path}: skipped, SmileBASIC 4's backup of an older file")
                elif path.suffix.lower() == ".p4a" or path.resolve() == output.resolve():
                    warn(f"{path}: skipped, an archive")
                else:
                    files.append(path)
        elif given.is_file():
            files.append(given)
        else:
            raise Refuse(f"{given}: no such file or folder")
    return files


def build_entries(files, warn):
    entries = {}
    for path in files:
        try:
            entry = read_file(path)
            entry.name = legal_name(entry.name, path, warn)
        except Skip as why:
            warn(f"{path}: SKIPPED: {why}")
            continue
        except OSError as error:
            raise Refuse(f"{path}: {error}") from error
        if entry.kind == "DAT" and PART_NAME.fullmatch(entry.name):
            raise Refuse(f"{path}: {entry.full_name} is the name of a part of an archive")
        other = entries.get(entry.full_name)
        if other is not None:
            raise Refuse(f"{path} and {other.source} would both be {entry.full_name}")
        entries[entry.full_name] = entry
    return sorted(entries.values(), key=lambda e: e.full_name)


def warn_limits(entry, warn):
    if entry.kind == "TXT":
        # Each UTF-16 unit is 1 to 3 bytes in the file.
        units = len(entry.data) // 2
        if units > SLOT_LIMIT:
            warn(
                f"{entry.full_name}: {units} characters: SAVEV can write it but LOAD into "
                f"a program slot stops at {SLOT_LIMIT}"
            )
        stored = sum(
            1 if u < 0x80 else 2 if u < 0x800 else 3
            for u in struct.unpack(f"<{units}H", entry.data)
        )
        if stored > BODY_LIMIT:
            warn(
                f"{entry.full_name}: {stored} bytes of UTF-8: over SmileBASIC 4's limit of "
                f"{BODY_LIMIT} unless it deflates to that; if not, SAVEV stops P4SENDX"
            )
    elif entry.kind == "DAT" and len(entry.data) > BODY_LIMIT:
        warn(
            f"{entry.full_name}: {len(entry.data)} bytes: over SmileBASIC 4's limit of "
            f"{BODY_LIMIT} unless it deflates to that; if not, SAVEV stops P4SENDX"
        )


def longest_matches(data, max_distance, max_length, shortest):
    """The longest match at each position, and a distance giving it."""
    size = len(data)
    lengths = [0] * size
    distances = [0] * size
    find = data.rfind
    current = 0
    distance = 0
    for at in range(1, size):
        limit = min(max_length, size - at)
        current = min(current - 1, limit) if current > 1 else 0
        if current < shortest:
            current = 0
        low = max(0, at - max_distance)
        while current < limit:
            if current and data[at + current] == data[at + current - distance]:
                current += 1
                continue
            want = current + 1 if current else shortest
            if want > limit:
                break
            # Any start before here will do, overlapping here included.
            found = find(data[at : at + want], low, at - 1 + want)
            if found < 0:
                break
            distance = at - found
            current = want
        lengths[at] = current
        distances[at] = distance
    return lengths, distances


def lz_compress(data):
    """The shortest list of tokens. A cost can only grow as more bytes are added in
    front: a parse of the rest is the same parse with its first token shortened. So
    the longest match of each kind at each position is the one to take, and the
    parse is exact."""
    size = len(data)
    short_lengths, short_distances = longest_matches(
        data, SHORT_DISTANCE, SHORT_LONGEST, SHORT_SHORTEST
    )
    long_lengths, long_distances = longest_matches(
        data, LONG_DISTANCE, LONG_LONGEST, LONG_SHORTEST
    )
    cost = [0] * (size + 1)
    for at in range(size - 1, -1, -1):
        best = cost[at + 1] + LITERAL_BITS
        length = short_lengths[at]
        if length:
            through = cost[at + length] + SHORT_BITS
            if through < best:
                best = through
        length = long_lengths[at]
        if length:
            through = cost[at + length] + LONG_BITS
            if through < best:
                best = through
        cost[at] = best

    out = bytearray()
    flag_at = 0
    flags_left = 0

    def flag(bit):
        nonlocal flag_at, flags_left
        if not flags_left:
            flag_at = len(out)
            out.append(0)
            flags_left = 8
        out[flag_at] |= bit << (8 - flags_left)
        flags_left -= 1

    at = 0
    while at < size:
        here = cost[at]
        short = short_lengths[at]
        if here == cost[at + 1] + LITERAL_BITS:
            flag(0)
            out.append(data[at])
            at += 1
        elif short and here == cost[at + short] + SHORT_BITS:
            flag(1)
            flag(0)
            token = (short_distances[at] - 1) | (short - SHORT_SHORTEST) << 10
            out += token.to_bytes(2, "little")
            at += short
        else:
            length = long_lengths[at]
            flag(1)
            flag(1)
            out += (long_distances[at] - 1).to_bytes(2, "little")
            out.append(length - LONG_SHORTEST)
            at += length
    return bytes(out)


def build_stream(image, compress):
    """(stream, id, method): the image, compressed if that is shorter, under its header."""
    payload = image
    method = METHOD_STORED
    if compress:
        packed = lz_compress(image)
        if len(packed) < len(image):
            payload = packed
            method = METHOD_LZ
    rest = struct.pack("<II", method, len(image)) + payload + bytes(-len(payload) % 4)
    identity = zlib.crc32(rest)
    return STREAM_MAGIC + struct.pack("<I", identity) + rest, identity, method


def part_originals(stream, identity, part_size):
    """What each part holds: prefix, piece, zero padding."""
    count = part_count_of(len(stream), part_size)
    originals = []
    for index in range(count):
        piece = stream[index * part_size : (index + 1) * part_size]
        data = struct.pack("<III", identity, index, count) + piece
        originals.append(data + bytes(-len(data) % 4))
    return originals


def build_file(stream, identity, part_size, originals, parity):
    count = len(originals)
    offset = struct.calcsize(FILE_HEADER) + struct.calcsize(ROW) * count
    out = bytearray(
        struct.pack(FILE_HEADER, FILE_MAGIC, identity, part_size, count, len(stream), parity)
    )
    for data in originals:
        out += struct.pack(ROW, offset, len(data), len(data), crc16(data), 0, 0)
        offset += len(data)
    for data in originals:
        out += data
    return bytes(out)


def stream_packets_multi(sent_length, parity, keyboards=7):
    """An NS link: 4 lead packets, groups of packets of 18 bits a keyboard, the same end.
    Parity off counts as 32 there, as the Flipper does it."""
    parity = parity or 32
    bits = (STREAM_OVERHEAD - 67 - 16 + sent_length) * 8
    group_bits = (parity - 1) * keyboards * 18
    return MULTI_LEAD_PACKETS + (bits + group_bits - 1) // group_bits * parity + END_PACKETS


def stream_packets(sent_length, parity=0):
    """Packets of a stream, as the Flipper makes them: 37 bits, with 5 more after an
    even one, then the clock values with no keys held. With parity, groups of that many
    packets, the last of each holding no data and 42 bits' worth of parity."""
    if parity:
        bits = (PARITY_DATA_OVERHEAD + sent_length) * 8
        group_bits = parity // 2 * (37 + 5 + 37) - (37 + 5)
        groups = (bits + group_bits - 1) // group_bits
        return PARITY_LEAD_PACKETS + groups * parity + END_PACKETS
    bits = (STREAM_OVERHEAD + sent_length) * 8
    pairs, rest = divmod(bits, 37 + 5 + 37)
    if rest == 0:
        extra = 0
    elif rest <= 42:
        extra = 1
    else:
        extra = 2
    return pairs * 2 + extra + END_PACKETS


def build_image(entries):
    image = bytearray(struct.pack("<I", len(entries)))
    for entry in entries:
        name = entry.full_name.encode("ascii")
        image += name.ljust(NAME_FIELD, b"\0")
        image += struct.pack("<IHH", len(entry.data), crc16(entry.data), 0)
        image += entry.data
        image += bytes(-len(entry.data) % 4)
    return bytes(image)


def part_count_of(size, part_size):
    return (size + part_size - 1) // part_size


def duration(seconds):
    seconds = int(seconds + 0.5)
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    if hours:
        return f"{hours} h {minutes:02d} min"
    return f"{minutes} min {secs:02d} s"


def main():
    parser = argparse.ArgumentParser(
        description="Pack files or a project for batch transfer to SmileBASIC 4."
    )
    parser.add_argument("inputs", nargs="+", type=Path, metavar="INPUT", help="files or folders")
    parser.add_argument("-o", "--output", type=Path, required=True, help="the .p4a file to write")
    parser.add_argument(
        "--no-compress", action="store_true", help="send the archive as it is"
    )
    parser.add_argument(
        "--parity",
        choices=list(PARITY_CHOICES),
        default=DEFAULT_PARITY,
        help="packets in a parity group, which can lose one: 16, 32 or 64, one parity packet "
        f"to about 79, 158 or 316 bytes, or off (default {DEFAULT_PARITY})",
    )
    parser.add_argument(
        "--part-size",
        type=int,
        default=DEFAULT_PART_SIZE,
        help=f"bytes in a part, a multiple of 4 (default {DEFAULT_PART_SIZE}; "
        "a failure costs a whole part)",
    )
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")

    warnings = []

    def warn(text):
        warnings.append(text)
        print(f"warning: {text}", file=sys.stderr)

    try:
        if args.part_size <= 0 or args.part_size % 4:
            raise Refuse(f"--part-size {args.part_size} is not a positive multiple of 4")
        if args.part_size > MAX_PART_SIZE:
            raise Refuse(f"--part-size {args.part_size} is over {MAX_PART_SIZE}")
        if args.part_size > DEFAULT_PART_SIZE:
            warn(
                "the larger the part size, the longer a failure takes to recover from. "
                "Use at your own risk."
            )
        if args.part_size < MIN_PART_SIZE:
            raise Refuse(f"--part-size {args.part_size} is under {MIN_PART_SIZE}")
        entries = build_entries(gather(args.inputs, args.output, warn), warn)
        if not entries:
            raise Refuse("nothing to pack")
        image = build_image(entries)
        if len(image) > MAX_ARCHIVE:
            raise Refuse(f"{len(image)} bytes is over SmileBASIC 4's limit of {MAX_ARCHIVE}")
        for entry in entries:
            warn_limits(entry, warn)
        if not args.no_compress:
            print(f"compressing {len(image)} bytes (about a minute for 2.6 MB)...", flush=True)
        stream, identity, method = build_stream(image, not args.no_compress)
        parts = part_count_of(len(stream), args.part_size)
        if parts > MAX_PARTS:
            raise Refuse(f"{parts} parts is over {MAX_PARTS}: choose a larger --part-size")
        originals = part_originals(stream, identity, args.part_size)
        parity = PARITY_CHOICES[args.parity]
        output = build_file(stream, identity, args.part_size, originals, parity)
        if len(output) > MAX_ARCHIVE:
            raise Refuse(f"{len(output)} bytes is over the Flipper's limit of {MAX_ARCHIVE}")
        args.output.write_bytes(output)
    except (Refuse, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"{'entry':38} {'bytes':>10}  from")
    for entry in entries:
        print(f"{entry.full_name:38} {len(entry.data):>10}  {entry.source.name}")
        for note in entry.notes:
            print(f"    note: {note}")
    sent = sum(len(data) for data in originals)
    print(f"archive id {identity:08X}: {len(entries)} entries, {len(image)} bytes")
    print(
        f"{parts} part(s) of {args.part_size} bytes, written to {args.output}: "
        f"{'LZ' if method == METHOD_LZ else 'not compressed'}"
    )
    percent = 100 * sent / len(image)
    print(f"to send: {len(image)} bytes before compression, {sent} after ({percent:.1f}%)")
    packets = sum(stream_packets(len(data), parity) for data in originals)
    if parity:
        plain = sum(stream_packets(len(data)) for data in originals)
        print(
            f"parity: one packet in {parity}, so one lost in a group is repaired: "
            f"{100 * (packets - plain) / plain:.1f}% more packets"
        )
    else:
        print("parity: off, so one lost packet fails its part")
    slow = packets * SLOW_PACKET_MS / 1000 + parts * PART_WAIT
    fast = packets * 0.2 / FAST_PACKETS_PER_200_MS + parts * PART_WAIT
    print(f"{packets} packets, {PART_WAIT} s after each part")
    print(f"expected at Slow: {duration(slow)}")
    print(f"expected at Fast: {duration(fast)}")
    for count in (6, 7):
        multi = sum(stream_packets_multi(len(data), parity, count) for data in originals)
        keyboards = multi * MULTI_PACKET_MS / 1000 + parts * PART_WAIT
        print(f"expected on NS{count - 5}, {multi} packets at {MULTI_PACKET_MS} ms: {duration(keyboards)}")
    if warnings:
        print(f"{len(warnings)} warning(s), above")
    return 0


if __name__ == "__main__":
    sys.exit(main())
