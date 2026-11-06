#!/usr/bin/env python3
"""Get files out of SmileBASIC 4 from the screenshots P4SEND makes.

In P4SEND (the SmileBASIC 4 side of Petit4Send, by Rei Software,
http://rei.to/petit4send.html, or P4SENDX, changed from it) choose
SCREENSHOT SEND and pick a file. It draws the file on the screen, a page at a
time: take a screenshot of each page, press a button for the next one, then
copy the pictures to a computer.

    p4recv.py -o OUT_DIR PICTURE_OR_FOLDER...

Pictures can be given in any order and may hold several files; page order
comes from each page's header. A picture with no P4SEND data, as a folder of
screenshots will hold, is skipped with a note. Each file is written once its
CRC has been checked:

    TXT:NAME  NAME.txt, as UTF-8
    GRP:NAME  NAME.png
    DAT:NAME  NAME.dat, little-endian 32-bit integers
    --raw     the bytes P4SEND sent, as NAME.txt.raw and so on, for any type

A file already in OUT_DIR is never replaced: a different one with that name
makes NAME~2.txt. The exit status is 1 if no P4SEND picture was found, or if a
file cannot be verified, and then:

    NAME.txt.bad          the CRC did not match; raw bytes, as far as they go
    NAME.unverified.txt   see below

P4SEND's uncompressed path leaves out the last 2 bytes of a text with an odd
number of characters, though its file size and CRC cover them. That is the
last character, and it can be worked out from the CRC because 16 bits of CRC
fix 16 unknown bits, but then nothing is left to check the file with. So such
a file is written as .unverified.txt, and the character is named in the output.
P4SENDX sends those last bytes, and such a file comes out as usual.

A picture must be at full size, with the page's 720x720 square in its top
left corner. JPEG is read from its brightness plane, without any conversion,
and a picture whose greys sit in video's limited range (16-235) is recognised.

Needs Pillow. Reads the format of P4SEND 1.2.2. The brightness limits and the
idea of reading JPEG's Y plane come from p4s (https://github.com/ysh86/p4s,
MIT License, Copyright (c) 2019 Yoshiharu Hara).
"""
import argparse
import io
import itertools
import re
import struct
import sys
import warnings
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, UnidentifiedImageError

MAGIC = 0x505A5332  # "PZS2"
HEADER_SIZE = 60
PAGE = 720  # the square of the screen that is drawn on
PIXELS_PER_GROUP = 5  # five pixels of seven levels hold 14 bits
GROUP_BITS = 14
PAGE_BYTES = PAGE * PAGE // PIXELS_PER_GROUP * GROUP_BITS // 8  # 181440
PAYLOAD_BYTES = PAGE_BYTES - HEADER_SIZE
LEVEL_LIMITS = (22, 64, 107, 149, 192, 234)  # upper bounds of the first six levels
LEVEL_VALUES = (0, 43, 85, 128, 170, 213, 255)  # the seven greys P4SEND draws
PICTURE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}
MAX_PICTURE_SIDE = 8192  # far beyond any screenshot; stops huge pictures early
MAX_COMBINATIONS = 64  # tries when copies of a page differ

LZ_DISTANCE_BITS = 10
LZ_LENGTH_BITS = 5



def level_table(limits):
    return bytes(
        next((i for i, limit in enumerate(limits) if value < limit), 6)
        for value in range(256)
    )


def fit_table(values):
    """For each brightness, how far (squared) it is from the nearest of these levels."""
    return [min((value - level) ** 2 for level in values) for value in range(256)]


# The same greys if something squeezed them into video's limited range, 16-235.
LIMITED_VALUES = tuple(round(16 + value * 219 / 255) for value in LEVEL_VALUES)
LIMITED_LIMITS = tuple(
    (low + high + 1) // 2 for low, high in zip(LIMITED_VALUES, LIMITED_VALUES[1:])
)
LEVEL_OF = level_table(LEVEL_LIMITS)
LIMITED_LEVEL_OF = level_table(LIMITED_LIMITS)
FULL_FIT = fit_table(LEVEL_VALUES)
LIMITED_FIT = fit_table(LIMITED_VALUES)


def crc_table():
    table = []
    for byte in range(256):
        value = byte
        for _ in range(8):
            value = 0xC7ED ^ (value >> 1) if value & 1 else value >> 1
        table.append(value)
    return table


CRC_TABLE = crc_table()


def crc_update(crc, data):
    table = CRC_TABLE
    for byte in data:
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc


def crc16(data):
    return crc_update(0xFFFF, data) ^ 0xFFFF


def missing_last_two(data, crc):
    """The two bytes that make the CRC of data + them come out as crc."""
    state = crc_update(0xFFFF, data)
    for first in range(256):
        after_first = crc_update(state, (first,))
        for second in range(256):
            if crc_update(after_first, (second,)) ^ 0xFFFF == crc:
                return bytes((first, second))
    raise ValueError("no two bytes give this CRC")  # cannot happen with a 16-bit CRC


def show(text):
    """Text from a picture, safe to print."""
    return "".join(c if c.isprintable() else f"\\x{ord(c):02x}" for c in text)


class NoPage(ValueError):
    """A readable picture, or no picture at all, that holds no P4SEND data."""


def read_levels(path):
    """A picture's page as levels 0-6, in the order they were drawn."""
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        try:
            image = Image.open(path)
        except UnidentifiedImageError as error:
            raise NoPage("not a picture") from error
        except (Image.DecompressionBombWarning, Image.DecompressionBombError) as error:
            raise NoPage("the picture is far too large") from error
    with image:
        width, height = image.size
        if width < PAGE or height < PAGE:
            raise NoPage(
                f"the picture is {width}x{height}, too small: P4SEND's are 1280x720"
            )
        if width > MAX_PICTURE_SIDE or height > MAX_PICTURE_SIDE:
            raise NoPage(f"the picture is {width}x{height}, too large")
        if image.format == "JPEG":
            # Asks the decoder for the stored Y plane itself.
            image.draft("L", None)
        square = image.convert("L").crop((0, 0, PAGE, PAGE))
        histogram = square.histogram()
        luma = square.tobytes()
    full_fit = sum(map(int.__mul__, histogram, FULL_FIT))
    limited_fit = sum(map(int.__mul__, histogram, LIMITED_FIT))
    levels = luma.translate(LIMITED_LEVEL_OF if limited_fit < full_fit else LEVEL_OF)
    # P4SEND goes down each column, left to right.
    return b"".join(levels[x::PAGE] for x in range(PAGE))


def page_bytes(levels):
    """The bytes of a page, and how many of its groups no P4SEND could draw."""
    digits = [levels[i :: PIXELS_PER_GROUP] for i in range(PIXELS_PER_GROUP)]
    groups = [
        a + b * 7 + c * 49 + d * 343 + e * 2401 for a, b, c, d, e in zip(*digits)
    ]
    impossible = sum(1 for value in groups if value >> GROUP_BITS)
    out = bytearray()
    # Four groups make 56 bits, seven whole bytes.
    for i in range(0, len(groups), 4):
        a, b, c, d = groups[i : i + 4]
        joined = (
            (a & 0x3FFF) | (b & 0x3FFF) << 14 | (c & 0x3FFF) << 28 | (d & 0x3FFF) << 42
        )
        out += joined.to_bytes(7, "little")
    return bytes(out), impossible


@dataclass
class Page:
    path: Path
    file_size: int
    crc: int
    name: str
    pages: int
    number: int
    xor: int
    compressed: bool
    payload: bytes  # with the XOR taken off
    impossible: int


def parse_page(path):
    """A picture's page. Raises NoPage if it holds none, ValueError if it is damaged."""
    data, impossible = page_bytes(read_levels(path))
    magic, file_size, crc = struct.unpack_from("<III", data)
    if magic != MAGIC:
        raise NoPage(
            "not a P4SEND page (no PZS2 mark in the top left corner; "
            "the picture must not be scaled or cropped)"
        )
    name = data[12:48].split(b"\0", 1)[0].decode("ascii", "replace")
    flags, stream, size = struct.unpack_from("<III", data, 48)
    pages = flags & 0xFF
    number = flags >> 8 & 0xFF
    xor = flags >> 16 & 0xFF
    compressed = flags >> 24 & 0xFF
    if (
        size != PAGE | PAGE << 16
        or crc > 0xFFFF
        or stream > PAYLOAD_BYTES
        or stream == 0
        or stream % 4
        or not pages
        or number >= pages
        or compressed > 1
        or (number < pages - 1 and stream != PAYLOAD_BYTES)
    ):
        raise ValueError("P4SEND page with a damaged header")
    payload = data[HEADER_SIZE : HEADER_SIZE + stream]
    payload = payload.translate(bytes(b ^ xor for b in range(256)))
    return Page(
        path, file_size, crc, name, pages, number, xor, bool(compressed), payload, impossible
    )


def unlz(data, size):
    """P4SEND's LZ. From the lowest bit up: 0 and a byte, or 1, distance - 1, length - 1."""
    out = bytearray()
    position = 0
    total_bits = len(data) * 8
    token_bits = 1 + LZ_DISTANCE_BITS + LZ_LENGTH_BITS
    distance_mask = (1 << LZ_DISTANCE_BITS) - 1
    length_mask = (1 << LZ_LENGTH_BITS) - 1
    from_bytes = int.from_bytes
    while len(out) < size:
        byte = position >> 3
        bits = from_bytes(data[byte : byte + 3], "little") >> (position & 7)
        if bits & 1:
            if position + token_bits > total_bits:
                break
            distance = (bits >> 1 & distance_mask) + 1
            length = (bits >> (1 + LZ_DISTANCE_BITS) & length_mask) + 1
            position += token_bits
            if distance > len(out):
                # Before the start of the file, its window holds zeros.
                zeros = min(length, distance - len(out))
                out += bytes(zeros)
                length -= zeros
            if length <= distance:
                start = len(out) - distance
                out += out[start : start + length]
            else:
                for _ in range(length):
                    out.append(out[-distance])
        else:
            if position + 9 > total_bits:
                break
            out.append(bits >> 1 & 0xFF)
            position += 9
    return bytes(out[:size])


def split_name(name):
    """The type (TXT, GRP or DAT) of a name from a picture, and a harmless stem.

    Only letters, digits and _ - @ . get through, so the stem cannot name another
    folder, and the callers add an extension, so it is never empty or hidden.
    """
    kind, _, rest = name.rpartition(":")
    kind = kind.upper()
    if kind not in ("TXT", "GRP", "DAT"):
        kind = "DAT"
    stem = re.sub(r"[^A-Za-z0-9_.@-]", "_", rest).strip(".") or "FILE"
    if re.fullmatch(r"(?i)(con|prn|aux|nul|com\d|lpt\d)(\..*)?", stem):
        stem = "_" + stem  # names Windows keeps for devices
    return kind, stem


def write_new(out_dir, stem, extension, data):
    """Writes a file that is new, or finds it already there. Returns its path."""
    for n in itertools.count(1):
        suffix = "" if n == 1 else f"~{n}"
        target = out_dir / f"{stem}{suffix}{extension}"
        try:
            with open(target, "xb") as file:
                file.write(data)
            return target
        except FileExistsError:
            if not target.is_symlink() and target.is_file() and target.read_bytes() == data:
                return target


def text_bytes(data):
    """A text as UTF-8, or None if it has half a character pair."""
    try:
        return data.decode("utf-16-le").encode("utf-8")
    except UnicodeError:
        return None


def grp_png(data):
    """A GRP file as PNG, or None if it does not hold a picture."""
    if len(data) < 4:
        return None
    width, height = struct.unpack_from("<HH", data)
    pixels = data[4:]
    if not width or not height or len(pixels) != width * height * 4:
        return None
    # In memory as bytes: blue, green, red, alpha.
    image = Image.frombuffer("RGBA", (width, height), pixels, "raw", "BGRA", 0, 1)
    out = io.BytesIO()
    image.save(out, "PNG")
    return out.getvalue()


def write_verified(out_dir, name, data, raw):
    """Writes a file whose CRC matched, and any note on it, returning the path."""
    kind, stem = split_name(name)
    lowered = kind.lower()
    if raw:
        return write_new(out_dir, stem, f".{lowered}.raw", data), None
    if kind == "TXT":
        text = text_bytes(data)
        if text is None:
            note = "not valid UTF-16, so kept as sent"
            return write_new(out_dir, stem, ".txt.utf16le", data), note
        return write_new(out_dir, stem, ".txt", text), None
    if kind == "GRP":
        png = grp_png(data)
        if png is None:
            note = "its size does not fit its pixels, so kept as sent"
            return write_new(out_dir, stem, ".grp.raw", data), note
        return write_new(out_dir, stem, ".png", png), None
    return write_new(out_dir, stem, ".dat", data), None


@dataclass
class Transfer:
    pages: dict = field(default_factory=dict)  # number -> the different copies


def collect(paths):
    pictures = []
    for path in paths:
        if path.is_dir():
            pictures += sorted(
                p for p in path.iterdir() if p.suffix.lower() in PICTURE_SUFFIXES
            )
        else:
            pictures.append(path)
    return pictures


def decode(pages):
    """A set of pages' bytes, the file as far as it can be rebuilt."""
    stream = b"".join(page.payload for page in pages)
    size = pages[0].file_size
    if pages[0].compressed:
        return unlz(stream, size)
    # P4SENDX pads the last word with zeros; P4SEND 1.2.2 sends no such bytes.
    if 0 < len(stream) - size < 4 and not any(stream[size:]):
        return stream[:size]
    return stream


def assemble(key, transfer, out_dir, raw):
    """Writes one file from its pages. True if it was verified."""
    name, file_size, crc, page_count, compressed = key
    label = show(name) or "(no name)"
    missing = [str(n + 1) for n in range(page_count) if n not in transfer.pages]
    if missing:
        print(
            f"{label}: page {', '.join(missing)} of {page_count} missing, nothing written",
            file=sys.stderr,
        )
        return False

    copies = [transfer.pages[n] for n in range(page_count)]
    tries = list(itertools.islice(itertools.product(*copies), MAX_COMBINATIONS))
    data = b""
    for pages in tries:
        data = decode(pages)
        if len(data) == file_size and crc16(data) == crc:
            target, note = write_verified(out_dir, name, data, raw)
            print(f"{label}: {file_size} bytes, {page_count} page(s), CRC good -> {target}")
            if note:
                print(f"{label}: {note}")
            return True
    pages = tries[0]
    data = decode(pages)
    kind, stem = split_name(name)
    impossible = sum(page.impossible for page in pages)
    damaged = f", and {impossible} damaged pixel groups were seen" if impossible else ""

    arrived = len(data)
    if not compressed and arrived == file_size // 4 * 4 and file_size % 4:
        lost = file_size - arrived
        print(
            f"{label}: P4SEND sent {arrived} of {file_size} bytes, as its uncompressed "
            f"path leaves out the last {lost}",
            file=sys.stderr,
        )
        if kind == "TXT" and lost == 2 and not impossible and len(tries) == 1:
            tail = missing_last_two(data, crc)
            code = tail[0] | tail[1] << 8
            full = text_bytes(data + tail)
            if full is not None:
                target = write_new(out_dir, stem, ".unverified.txt", full)
                print(
                    f"{label}: UNVERIFIED. Its last character, from the CRC, is "
                    f"U+{code:04X} '{show(chr(code))}' if all else arrived intact, but "
                    f"that uses up the CRC, so nothing checks this file -> {target}",
                    file=sys.stderr,
                )
                return False
    elif not compressed and arrived != file_size:
        print(
            f"{label}: the pages hold {arrived} bytes for a file of {file_size}",
            file=sys.stderr,
        )
    elif compressed and arrived != file_size:
        print(f"{label}: unpacked to {arrived} of {file_size} bytes", file=sys.stderr)

    target = write_new(out_dir, stem, f".{kind.lower()}.bad", data)
    print(
        f"{label}: CHECK FAILED{damaged}. The picture may be damaged (JPEG at too low "
        f"a quality, or edited), raw bytes -> {target}",
        file=sys.stderr,
    )
    return False


def add_page(transfers, page):
    """Files a page under its transfer, keeping one copy of each different version."""
    key = (page.name, page.file_size, page.crc, page.pages, page.compressed)
    copies = transfers.setdefault(key, Transfer()).pages.setdefault(page.number, [])
    for copy in copies:
        if copy.payload == page.payload:
            print(
                f"{page.path}: same as {copy.path}, page {page.number + 1} "
                f"of {show(page.name)}; ignored"
            )
            return
    if copies:
        print(
            f"{page.path}: differs from {copies[0].path}, both page {page.number + 1} "
            f"of {show(page.name)}; the CRC will choose",
            file=sys.stderr,
        )
    copies.append(page)


def main():
    parser = argparse.ArgumentParser(
        description="Get files out of SmileBASIC 4 from P4SEND's screenshots."
    )
    parser.add_argument("pictures", nargs="+", type=Path, help="pictures or folders")
    parser.add_argument("-o", "--output", type=Path, default=Path("."), help="output folder")
    parser.add_argument(
        "--raw", action="store_true", help="write the bytes as sent, without converting"
    )
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")

    transfers = {}
    fine = True
    for path in collect(args.pictures):
        try:
            add_page(transfers, parse_page(path))
        except NoPage as error:
            print(f"{path}: skipped: {error}", file=sys.stderr)
        except Exception as error:  # Pillow raises many types for broken files
            print(f"{path}: {error}", file=sys.stderr)
            fine = False

    if not transfers:
        print("no P4SEND pictures found", file=sys.stderr)
        return 1
    try:
        args.output.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        print(f"{args.output}: {error}", file=sys.stderr)
        return 1
    for key, transfer in transfers.items():
        try:
            fine = assemble(key, transfer, args.output, args.raw) and fine
        except OSError as error:
            print(f"{show(key[0])}: {error}", file=sys.stderr)
            fine = False
    return 0 if fine else 1


if __name__ == "__main__":
    sys.exit(main())
