"""Shared code for testing p4recv.py: a small Python copy of P4SEND's drawing."""
import re
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
PC = HERE.parent / "pc"
P4RECV = PC / "p4recv.py"
sys.path.insert(0, str(PC))
import p4recv  # noqa: E402

COLOURS = (0, 43, 85, 128, 170, 213, 255)


def recv(pictures, out, *options):
    """Runs p4recv.py: (exit status, stdout + stderr). A traceback fails the test."""
    r = subprocess.run(
        [sys.executable, str(P4RECV), "-o", str(out), *options, *map(str, pictures)],
        capture_output=True,
        text=True,
    )
    assert "Traceback" not in r.stderr, r.stderr
    return r.returncode, r.stdout + r.stderr


def encode_page(payload, name, file_size, crc, pages, number, xor, compressed, width=1280):
    """Draws a page as P4SEND does: payload a multiple of 4, unXORed. Returns an L picture."""
    header = bytearray(60)
    struct.pack_into("<III", header, 0, p4recv.MAGIC, file_size, crc)
    header[12 : 12 + len(name)] = name
    flags = pages | number << 8 | xor << 16 | compressed << 24
    struct.pack_into("<III", header, 48, flags, len(payload), 720 | 720 << 16)
    stream = bytes(header) + payload.translate(bytes(b ^ xor for b in range(256)))
    stream += bytes(p4recv.PAGE_BYTES - len(stream))
    column_bytes = bytearray(720 * 720)
    # Group by group from the low end, in chunks to stay fast.
    groups = []
    for i in range(0, len(stream), 7):
        chunk = int.from_bytes(stream[i : i + 7], "little")
        groups += [chunk >> s & 0x3FFF for s in (0, 14, 28, 42)]
    index = 0
    for t in groups:
        for _ in range(5):
            column_bytes[index] = COLOURS[t % 7]
            t //= 7
            index += 1
    # column_bytes goes down each column; a picture goes along each row.
    rows = b"".join(
        bytes(column_bytes[y::720]) + bytes(width - 720) for y in range(720)
    )
    return Image.frombytes("L", (width, 720), rows)
