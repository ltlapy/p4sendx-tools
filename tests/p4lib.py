"""Shared code for the tests: a Python encoder of the Petit4Send stream and of the
archive format, written apart from the tools and the Flipper's code."""
import math
import os
import re
import struct
import subprocess
import sys
import zlib
from collections import namedtuple
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
PC = HERE.parent / "pc"
P4PACK = PC / "p4pack.py"

KEYCODES = [
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 54, 55, 56, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88, 89,
    90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
    133, 134, 135, 137, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
    198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215,
    216, 217, 218, 219, 220, 221, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 232,
    233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249,
]
assert len(KEYCODES) == 195
SLOW_MS = 24

_CHOOSE = [[0] * 196 for _ in range(7)]
for n in range(196):
    _CHOOSE[0][n] = 1
    for k in range(1, 7):
        _CHOOSE[k][n] = 0 if n == 0 else _CHOOSE[k - 1][n - 1] + _CHOOSE[k][n - 1]


def choose_keys(value):
    keys = []
    index = 194
    for k in range(6, 0, -1):
        while _CHOOSE[k][index] > value:
            index -= 1
        value -= _CHOOSE[k][index]
        keys.append(KEYCODES[index])
        index -= 1
    return keys


_TABLE = []
for byte in range(256):
    value = byte
    for _ in range(8):
        value = 0xC7ED ^ (value >> 1) if value & 1 else value >> 1
    _TABLE.append(value)


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc = _TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFF


LZ_FORMATS = {1: (10, 5)}  # for single files: distance and length bits


def lz_compress(data, mode=1):
    """A plain greedy LZ of the tests' own, so that P4SENDX is not only fed by p4pack.py."""
    distance_bits, length_bits = LZ_FORMATS[mode]
    max_distance = (1 << distance_bits) - 1
    max_length = 1 << length_bits
    bits = 0
    count = 0
    at = 0
    table = {}
    while at < len(data):
        best = 0
        dist = 0
        key = data[at : at + 3]
        for start in reversed(table.get(key, [])[-16:]):
            d = at - start
            if d > max_distance:
                break
            n = 0
            while n < max_length and at + n < len(data) and data[start + n] == data[at + n]:
                n += 1
            if n > best:
                best, dist = n, d
                if n == max_length:
                    break
        step = 1
        if best >= 3:
            bits |= (1 | (dist - 1) << 1 | (best - 1) << (1 + distance_bits)) << count
            count += 1 + distance_bits + length_bits
            step = best
        else:
            bits |= data[at] << (count + 1)
            count += 9
        for i in range(at, at + step):
            table.setdefault(data[i : i + 3], []).append(i)
        at += step
    return bits.to_bytes((count + 7) // 8, "little")


def lz_expand(sent, size, mode):
    """An independent reader of the format, from the spec. The window starts as zeros."""
    distance_bits, length_bits = LZ_FORMATS[mode]
    value = int.from_bytes(sent, "little")
    total = len(sent) * 8
    position = 0
    out = bytearray()
    while len(out) < size:
        assert position < total, "the stream ends early"
        if value >> position & 1:
            d = (value >> position + 1 & (1 << distance_bits) - 1) + 1
            n = (value >> position + 1 + distance_bits & (1 << length_bits) - 1) + 1
            position += 1 + distance_bits + length_bits
            assert 0 < d <= len(out), "reaches before the start"
            for _ in range(n):
                out.append(out[-d])
        else:
            out.append(value >> position + 1 & 0xFF)
            position += 9
    assert len(out) == size, "a match runs past the end"
    assert (total - position) < 8 and not value >> position, "bytes left over"
    return bytes(out)


def stream_of(name, data, width=0, height=0, compress=0, crc=None, sent=None):
    """A whole stream's bytes: name such as "DAT:P4A00000", data the original."""
    compress = int(compress)
    if sent is None:
        sent = lz_compress(data, compress) if compress else data
    head = bytes(16) + b"\x01" * 50 + b"\0"
    head += name.encode("ascii")[:36].ljust(36, b"\0")
    head += struct.pack("<IHHBI", len(data), width, height, compress, len(sent))
    value = crc16(data) if crc is None else crc
    return head + sent + struct.pack("<H", value) + bytes(16)


def packets(stream):
    """(mods as P4SEND's KEYBOARD reads them, mouse, keys) for each packet."""
    value = int.from_bytes(stream, "little")
    total = len(stream) * 8
    at = 0
    out = []
    clock = 0
    mouse = 0
    while at < total:
        packet = value >> at & (1 << 37) - 1
        at += 37
        mk = 0
        if clock & 1:
            mk |= 1
        if clock & 2:
            mk |= 2
        if packet >> 36:
            mk |= 8
        if not clock & 1:
            mouse = value >> at & 31
            at += 5
        out.append((mk, mouse, choose_keys(packet & (1 << 36) - 1)))
        clock += 1
    # After the stream, clock values with no keys held, as the PC program ends.
    for mk in (3, 2, 1, 0):
        out.append((mk, 0, []))
    return out


def stream_body(name, data, crc=None):
    """What a parity stream's groups hold: name, sizes, the bytes, the CRC."""
    head = name.encode("ascii")[:36].ljust(36, b"\0") + struct.pack("<IHHBI", len(data), 0, 0, 0, len(data))
    return head + data + struct.pack("<H", crc16(data) if crc is None else crc)


def parity_packets(name, data, group, crc=None, bad_parity=()):
    """A parity stream's packets, from the spec and apart from the Flipper's code: the
    leader, G, zero bits up to bit 553, then groups of (mk, mouse, keys) in which the last
    packet is the XOR of the others, an even packet counting as having no mouse bits.
    bad_parity lists groups whose parity packet is wrong, as if a bit were misread."""
    assert group % 2 == 0
    lead = bytes(16) + b"\x01" * 50 + b"\0" + bytes([group]) + bytes(2)
    value = int.from_bytes(lead, "little")
    out = []
    at = 0
    mouse = 0
    clock = 0

    def add(packet):
        nonlocal clock
        mk = (clock & 3) | (8 if packet >> 36 else 0)
        out.append((mk, mouse, choose_keys(packet & (1 << 36) - 1)))
        clock += 1

    for _ in range(14):
        packet = value >> at & (1 << 37) - 1
        at += 37
        if clock % 2 == 0:
            mouse = value >> at & 31
            at += 5
        add(packet)
    assert at == 553
    body = stream_body(name, data, crc)
    value = int.from_bytes(body, "little")
    total = len(body) * 8
    at = 0
    number = 0
    while at < total:
        xor_value = xor_mouse = 0
        for index in range(group - 1):
            packet = value >> at & (1 << 37) - 1
            at += 37
            xor_value ^= packet
            if index % 2 == 0:
                if index == group - 2:
                    mouse = xor_mouse
                else:
                    mouse = value >> at & 31
                    at += 5
                    xor_mouse ^= mouse
            add(packet)
        add(xor_value ^ (1 if number in bad_parity else 0))
        number += 1
    for mk in (3, 2, 1, 0):
        out.append((mk, 0, []))
    return out


MULTI_KEYS = sorted(k for k in KEYCODES if not 40 <= k <= 44 and k != 88)
assert len(MULTI_KEYS) == 189


def multi_keys(values):
    """The keys of 6 or 7 values, each the number of a combination of 6 of a keyboard's 27,
    worked from the definition, slowly, apart from the Flipper's greedy table. 2^18 is the
    code for the value before."""
    keys = []
    for board, value in enumerate(values):
        assert 0 <= value <= 1 << 18
        chosen = []
        for k in range(6, 0, -1):
            index = 26
            while math.comb(index, k) > value:
                index -= 1
            value -= math.comb(index, k)
            chosen.append(MULTI_KEYS[board * 27 + index])
        keys += sorted(chosen)
    return keys


def multi_packets(name, data, group, crc=None, body=None, boards=7):
    """A stream of an NS link from the spec: 4 lead packets, groups, the end. A value is
    masked for its place in its group, and one still equal to the code just sent on that
    keyboard goes as the code 2^18."""
    out = []
    clock = 0
    last = [None] * boards

    def add(values, place=None):
        nonlocal clock
        codes = []
        for board, value in enumerate(values):
            if place is not None:
                value ^= ((place + 1) * 40503 + (board + 1) * 25931) % (1 << 18)
                code = 1 << 18 if value == last[board] else value
            else:
                code = value
            last[board] = code
            codes.append(code)
        out.append((clock & 3, 0, multi_keys(codes)))
        clock += 1

    for left in (3, 2, 1, 0):
        add([0x2B4A5, group, left | boards << 8, 0, 0, 0, 0][:boards])
    body = stream_body(name, data, crc) if body is None else body
    value = int.from_bytes(body, "little")
    total = len(body) * 8
    at = 0
    while at < total:
        xor = [0] * boards
        for place in range(group - 1):
            values = []
            for board in range(boards):
                values.append(value >> at & (1 << 18) - 1)
                xor[board] ^= values[board]
                at += 18
            add(values, place)
        add(xor, group - 1)
    for mk in (3, 2, 1, 0):
        out.append((mk, 0, []))
    return out


def host_multi(host, *args, boards=7):
    """Packets of the Flipper's real code on an NS link, as (mk, mouse, keys)."""
    env = dict(os.environ, P4S_LINK=str(boards))
    r = subprocess.run([host, *map(str, args)], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stderr
    rows = []
    for line in r.stdout.split("\n"):
        if line.startswith("DATA "):
            at, mk, mouse, *keys = (int(v) for v in line[5:].split(","))
            rows.append((mk, mouse, [k for k in keys if k]))
    return rows


Part = namedtuple("Part", "name data compress sent")


class TokenWriter:
    """Method 1's bytes, written apart from p4pack.py's, token by token."""

    def __init__(self):
        self.out = bytearray()
        self.left = 0
        self.at = 0
        self.long_tokens = []

    def flag(self, bit):
        if not self.left:
            self.at = len(self.out)
            self.out.append(0)
            self.left = 8
        self.out[self.at] |= bit << (8 - self.left)
        self.left -= 1

    def literal(self, byte):
        self.flag(0)
        self.out.append(byte)

    def short(self, distance, length):
        assert 1 <= distance <= 1024 and 2 <= length <= 65
        self.flag(1)
        self.flag(0)
        self.out += struct.pack("<H", distance - 1 | (length - 2) << 10)

    def long(self, distance, length):
        assert 1 <= distance <= 65536 and 3 <= length <= 258
        self.flag(1)
        self.flag(1)
        self.long_tokens.append(len(self.out))
        self.out += struct.pack("<HB", distance - 1, length - 3)


def stream_compress(image):
    """A plain greedy packer of the tests' own, so that P4SENDX is not only fed by p4pack.py."""
    writer = TokenWriter()
    table = {}
    at = 0
    while at < len(image):
        best = dist = 0
        for start in reversed(table.get(image[at : at + 3], [])[-16:]):
            if at - start > 65536:
                break
            n = 0
            while n < 258 and at + n < len(image) and image[start + n] == image[at + n]:
                n += 1
            if n > best:
                best, dist = n, at - start
        step = 1
        if best >= 3 and dist <= 1024 and best <= 65:
            writer.short(dist, best)
            step = best
        elif best >= 3:
            writer.long(dist, best)
            step = best
        else:
            writer.literal(image[at])
        for i in range(at, at + step):
            table.setdefault(image[i : i + 3], []).append(i)
        at += step
    return bytes(writer.out)


def scripted_compress(image, matches):
    """Literals, but for the matches asked for: {position: (distance, length, long?)}.
    The writer also says where its long tokens are."""
    writer = TokenWriter()
    at = 0
    while at < len(image):
        if at in matches:
            distance, length, is_long = matches[at]
            assert image[at - distance : at - distance + length] == image[at : at + length] or all(
                image[at + i] == image[at + i - distance] for i in range(length)
            ), at
            (writer.long if is_long else writer.short)(distance, length)
            at += length
        else:
            writer.literal(image[at])
            at += 1
    return bytes(writer.out), writer.long_tokens


def stream_expand(payload, size):
    """An independent reader of method 1, from the spec. Nothing may reach before the
    start or run past the end."""
    out = bytearray()
    at = 0
    flags = left = 0

    def flag():
        nonlocal at, flags, left
        if not left:
            flags = payload[at]
            at += 1
            left = 8
        bit = flags & 1
        flags >>= 1
        left -= 1
        return bit

    while len(out) < size:
        if not flag():
            out.append(payload[at])
            at += 1
            continue
        if flag():
            distance = payload[at] + (payload[at + 1] << 8) + 1
            length = payload[at + 2] + 3
            at += 3
        else:
            token = payload[at] + (payload[at + 1] << 8)
            at += 2
            distance = (token & 1023) + 1
            length = (token >> 10) + 2
        assert distance <= len(out), "reaches before the start"
        for _ in range(length):
            out.append(out[-distance])
    assert len(out) == size, "a match runs past the end"
    assert not any(payload[at:]) and len(payload) - at < 8, "bytes left over"
    return bytes(out)


def pack_image(entries):
    """A small archive maker of the tests' own, against which p4pack.py is compared."""
    body = struct.pack("<I", len(entries))
    for name, data in entries:
        body += name.encode().ljust(36, b"\0") + struct.pack("<IHH", len(data), crc16(data), 0)
        body += data + bytes(-len(data) % 4)
    return body


def pack_stream(image, payload=None, magic=b"P4A3", method=None, size=None):
    """The stream for an image: stored, or with the method 1 payload given."""
    if method is None:
        method = 0 if payload is None else 1
    payload = image if payload is None else payload
    rest = struct.pack("<II", method, len(image) if size is None else size) + payload
    rest += bytes(-len(rest) % 4)
    return magic + struct.pack("<I", zlib.crc32(rest)) + rest


def pack_archive(entries, compress=False):
    """The stream for these entries."""
    image = pack_image(entries)
    return pack_stream(image, stream_compress(image) if compress else None)


def split_stream(stream, part_size):
    """A stream's parts, as a .p4a holds them."""
    identity = stream[4:8]
    count = (len(stream) + part_size - 1) // part_size
    parts = []
    for index in range(count):
        piece = stream[index * part_size : (index + 1) * part_size]
        data = identity + struct.pack("<II", index, count) + piece
        data += bytes(-len(data) % 4)
        parts.append(Part(f"DAT:P4A{index:05d}", data, 0, data))
    return parts


def read_stream(file_bytes):
    """A .p4a's rows, checked against the spec's layout, and the stream they hold."""
    magic, identity, part_size, count, stream_size, parity = struct.unpack_from("<4sIIIII", file_bytes)
    assert magic == b"P4A4", magic
    assert parity in (0, 16, 32, 64), parity
    rows = []
    stream = bytearray()
    next_offset = 24 + 16 * count
    assert count == (stream_size + part_size - 1) // part_size
    for index in range(count):
        offset, sent_length, original_length, crc, mode, zero = struct.unpack_from(
            "<IIIHBB", file_bytes, 24 + 16 * index
        )
        assert offset == next_offset and zero == 0 and mode == 0
        assert sent_length == original_length
        next_offset += sent_length
        data = file_bytes[offset : offset + sent_length]
        assert crc16(data) == crc, f"part {index}"
        assert struct.unpack_from("<III", data) == (identity, index, count)
        piece = data[12:]
        if index < count - 1:
            assert len(piece) == part_size
        stream += piece
        rows.append(Part(f"DAT:P4A{index:05d}", data, 0, data))
    assert next_offset == len(file_bytes)
    assert len(stream) == stream_size and stream_size % 4 == 0
    return rows, bytes(stream)


def read_p4a2(file_bytes):
    """A .p4a's rows and its image, every layer checked."""
    rows, stream = read_stream(file_bytes)
    magic, identity, method, image_size = struct.unpack_from("<4sIII", stream)
    assert magic == b"P4A3", magic
    assert file_bytes[4:8] == stream[4:8]
    assert identity == zlib.crc32(stream[8:])
    if method == 0:
        assert image_size == len(stream) - 16
        return rows, stream[16:]
    assert method == 1, method
    return rows, stream_expand(stream[16:], image_size)


def part_streams(archive, part_size=None):
    """The parts of a .p4a as it stands, or of a stream at that part size."""
    if archive[:4] == b"P4A4":
        return read_stream(archive)[0]
    return split_stream(archive, part_size)
