#!/usr/bin/env python3
"""Bad input to p4recv.py, on pictures from the Python copy of P4SEND's drawing,
whose faithfulness is checked first against a real P4SEND picture.

    test_robust.py WORK_DIR [REAL_DAT_SHOT REAL_DAT_ORIGINAL]
"""
import random
import shutil
import struct
import sys
import time
from pathlib import Path

from PIL import Image

from recvlib import encode_page, recv
from p4recv import PAYLOAD_BYTES, crc16, parse_page, read_levels, page_bytes

WORK = Path(sys.argv[1])
__import__("workdir").guard(WORK); shutil.rmtree(WORK, ignore_errors=True)
WORK.mkdir(parents=True)
rng = random.Random(9)
failures = 0
case_no = 0


def check(case, condition, detail=""):
    global failures
    failures += not condition
    print(f"{'OK  ' if condition else 'FAIL'} {case} {detail}", flush=True)


def fresh():
    global case_no
    case_no += 1
    folder = WORK / f"c{case_no}"
    folder.mkdir()
    return folder


def picture(folder, tag, payload, name, size=None, crc=None, pages=1, number=0, xor=0x5A, compressed=0):
    payload_size = len(payload) if size is None else size
    checksum = crc16(payload) if crc is None else crc
    path = folder / f"{tag}.png"
    encode_page(payload, name, payload_size, checksum, pages, number, xor, compressed).save(path)
    return path


# The copy of the drawing against a real picture.
if len(sys.argv) > 3:
    real = Path(sys.argv[2])
    original = Path(sys.argv[3]).read_bytes()
    page = parse_page(real)
    mine = fresh() / "mine.png"
    encode_page(original, page.name.encode(), page.file_size, page.crc, 1, 0, page.xor, 0).save(mine)
    check("python drawing == real P4SEND levels", read_levels(mine) == read_levels(real))
    began = time.perf_counter()
    for _ in range(5):
        parse_page(real)
    print(f"     one full page: {(time.perf_counter() - began) / 5:.3f} s")

# Names from a picture must not leave the folder.
body = bytes(range(8))
for name in [
    b"../X", b"TXT:/ABS", b"", b"TXT:", b"DAT:../../E", b"GRP:..", b"../../E:Y",
    b"X/../../E:Y", b"TXT:..\\..\\W", b"DAT:CON", b"TXT:\x1b[2J", b"\xff\xfe:\xfd", b"A:B:C", b"TXT:.hidden",
]:
    folder = fresh()
    out = folder / "deep" / "out"
    pic = picture(folder, "p", body if b"TXT" not in name else body, name)
    status, text = recv([pic], out)
    produced = [p for p in folder.rglob("*") if p.is_file() and p != pic]
    inside = all(out in p.parents and p.parent == out for p in produced)
    clean = "\x1b" not in text
    check(f"name {name!r}", inside and len(produced) == 1 and clean, f"-> {[p.name for p in produced]} exit={status}")

# Two different files of one name, a re-run, and a file left in the folder.
folder = fresh()
out = folder / "out"
first = picture(folder, "a", b"AAAA" * 2, b"DAT:SAME")
second = picture(folder, "b", b"BBBB" * 2, b"DAT:SAME")
status, text = recv([first, second], out)
check("two files of one name", status == 0 and {p.read_bytes() for p in out.iterdir()} == {b"AAAA" * 2, b"BBBB" * 2}, str(sorted(p.name for p in out.iterdir())))
count = len(list(out.iterdir()))
status, text = recv([first, second], out)
check("run again adds nothing", status == 0 and len(list(out.iterdir())) == count)

# A symbolic link left in the output folder is not written through.
folder = fresh()
out = folder / "out"
out.mkdir()
outside = folder / "outside"
outside.write_bytes(b"keep")
(out / "LINK.dat").symlink_to(outside)
pic = picture(folder, "p", body, b"DAT:LINK")
status, text = recv([pic], out)
check("symlink in the output folder", status == 0 and outside.read_bytes() == b"keep" and (out / "LINK~2.dat").read_bytes() == body)

# Size not a multiple of 4: only TXT can recover, and never with status 0.
folder = fresh()
data = rng.randbytes(40)
tail_text = "끝".encode("utf-16-le")
full = data + tail_text
pic = picture(folder, "p", data, b"DAT:SHORT2", size=42, crc=crc16(full))
status, text = recv([pic], folder / "out")
check("DAT lost 2 bytes: bad, partial", status == 1 and (folder / "out" / "SHORT2.dat.bad").read_bytes() == data, text.strip().split("\n")[0])
for lost in (1, 3):
    folder = fresh()
    full = data + rng.randbytes(lost)
    pic = picture(folder, "p", data, b"TXT:ODD", size=40 + lost, crc=crc16(full))
    status, text = recv([pic], folder / "out")
    check(f"TXT lost {lost} byte(s): bad, partial", status == 1 and (folder / "out" / "ODD.txt.bad").read_bytes() == data)

# Lost 2 bytes, but the arrived part damaged: says UNVERIFIED, exit 1; it cannot know.
folder = fresh()
text_body = "가나다라마바사아자차".encode("utf-16-le")
full = text_body + "카".encode("utf-16-le")
pic = picture(folder, "p", text_body, b"TXT:DMG", size=len(full), crc=crc16(full))
status, text = recv([pic], folder / "out")
check("TXT lost 2 bytes, intact", status == 1 and (folder / "out" / "DMG.unverified.txt").read_text("utf-8") == full.decode("utf-16-le"))

# A wrong CRC.
folder = fresh()
pic = picture(folder, "p", body, b"DAT:WRONG", crc=crc16(body) ^ 1)
status, text = recv([pic], folder / "out")
check("wrong CRC: .bad, exit 1", status == 1 and (folder / "out" / "WRONG.dat.bad").read_bytes() == body and "CHECK FAILED" in text)

# Damage in the middle of an LZ stream must be noticed: flip a pixel in a real-shaped page.
folder = fresh()
original = (b"abcdefgh" * 100)
stream = bytearray()
# Literal-only LZ stream built by hand: 0 + byte for each byte, then padded.
bits = 0
position = 0
for byte in original:
    bits |= byte << (position + 1)
    position += 9
packed = bits.to_bytes((position + 31) // 32 * 4, "little")
pic = picture(folder, "p", packed, b"DAT:LZOK", size=len(original), crc=crc16(original), compressed=1)
status, text = recv([pic], folder / "out")
check("hand-made LZ", status == 0 and (folder / "out" / "LZOK.dat").read_bytes() == original)
image = Image.open(pic).copy()
image.putpixel((3, 100), 255 - image.getpixel((3, 100)))
damaged = folder / "damaged.png"
image.save(damaged)
status, text = recv([damaged], folder / "out2")
check("one damaged pixel, LZ", status == 1 and "CHECK FAILED" in text, text.strip()[:70])

# LZ whose lengths pass the end, whose distances pass the start, and truncation.
folder = fresh()
tokens = 0
position = 0
for _ in range(50):
    tokens |= (1 | 1023 << 1 | 31 << 11) << position  # far back: zeros, long
    position += 16
packed = tokens.to_bytes((position + 31) // 32 * 4, "little")
pic = picture(folder, "p", packed, b"DAT:FAR", size=10_000_000, crc=0, compressed=1)
status, text = recv([pic], folder / "out")
written = (folder / "out" / "FAR.dat.bad").read_bytes()
check("LZ bomb-like size is bounded", status == 1 and len(written) == 1600, f"{len(written)} bytes, {text.strip()[:60]}")

# Pages: missing, duplicate, one of two damaged.
a = rng.randbytes(PAYLOAD_BYTES)
b = rng.randbytes(1000)
whole = a + b
folder = fresh()
crc = crc16(whole)
p0 = picture(folder, "p0", a, b"DAT:TWO", size=len(whole), crc=crc, pages=2, number=0, xor=1)
p1 = picture(folder, "p1", b, b"DAT:TWO", size=len(whole), crc=crc, pages=2, number=1, xor=2)
status, text = recv([p1, p0], folder / "o1")
check("reversed order, XOR differs per page", status == 0 and (folder / "o1" / "TWO.dat").read_bytes() == whole)
status, text = recv([p1], folder / "o2")
check("missing page", status == 1 and "page 1 of 2 missing" in text and not (folder / "o2").exists() or not list((folder / "o2").iterdir()), text.strip())
status, text = recv([p0, p1, p0], folder / "o3")
check("duplicate picture", status == 0 and (folder / "o3" / "TWO.dat").read_bytes() == whole and "same as" in text, text.strip().split("\n")[0][-60:])
image = Image.open(p1).copy()
image.putpixel((2, 300), 255 - image.getpixel((2, 300)))
bad1 = folder / "bad1.png"
image.save(bad1)
status, text = recv([p0, bad1, p1], folder / "o4")
check("damaged copy and good copy: CRC chooses", status == 0 and (folder / "o4" / "TWO.dat").read_bytes() == whole and "differs" in text)
status, text = recv([p0, bad1], folder / "o5")
check("only the damaged copy", status == 1 and (folder / "o5" / "TWO.dat.bad").exists())

# Groups past the end of the data: P4SEND leaves them black, and nothing reads them.
image = Image.open(p1).copy()
for x in range(10, 720):
    for y in range(0, 720, 3):
        image.putpixel((x, y), rng.randrange(256))
noisy = folder / "noisy_end.png"
image.save(noisy)
status, text = recv([p0, noisy], folder / "o6")
check("noise after the data's end", status == 0 and (folder / "o6" / "TWO.dat").read_bytes() == whole)

# --raw keeps the bytes of a text.
folder = fresh()
raw_text = "한글".encode("utf-16-le")
pic = picture(folder, "p", raw_text, b"TXT:R")
status, text = recv([pic], folder / "o", "--raw")
check("--raw", status == 0 and (folder / "o" / "R.txt.raw").read_bytes() == raw_text)

# A picture wider or taller than the screen keeps its square in the top left.
pic = Image.open(pic)
canvas = Image.new("L", (1920, 1080), 0)
canvas.paste(pic.crop((0, 0, 720, 720)), (0, 0))
canvas.save(folder / "big.png")
status, text = recv([folder / "big.png"], folder / "o2")
check("larger canvas", status == 0 and (folder / "o2" / "R.txt").read_text("utf-8") == "한글")

# A picture scaled to another size cannot be read, and says so.
pic.resize((1920, 1080)).save(folder / "scaled.png")
status, text = recv([folder / "scaled.png"], folder / "o3")
check("scaled picture", status == 1 and "not a P4SEND page" in text)

# Unusable pictures.
folder = fresh()
Image.new("RGB", (640, 360), "white").save(folder / "small.png")
status, text = recv([folder / "small.png"], folder / "o")
check("too small", status == 1 and "too small" in text, text.strip()[-60:])
Image.new("RGB", (1280, 720), (10, 200, 30)).save(folder / "other.jpg", quality=90)
status, text = recv([folder / "other.jpg"], folder / "o")
check("not a P4SEND picture", status == 1 and "not a P4SEND page" in text, text.strip()[-60:])
(folder / "text.png").write_text("this is no picture")
status, text = recv([folder / "text.png"], folder / "o")
check("not a picture at all", status == 1, text.strip()[-60:])
data = (folder / "other.jpg").read_bytes()
(folder / "cut.jpg").write_bytes(data[: len(data) // 2])
status, text = recv([folder / "cut.jpg"], folder / "o")
check("cut JPEG", status == 1, text.strip()[-60:])
status, text = recv([folder / "nothing.png"], folder / "o")
check("no such file", status == 1, text.strip()[-60:])
empty = folder / "empty"
empty.mkdir()
status, text = recv([empty], folder / "o")
check("empty folder", status == 1 and "no P4SEND pictures" in text, text.strip())


def bomb_png(path, width, height):
    """A PNG header claiming a huge size, its pixels never needed."""
    import zlib

    def chunk(kind, body):
        crc = zlib.crc32(kind + body)
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    head = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(b"\0" * 100)) + chunk(b"IEND", b"")
    )


for width, height, tag in ((20000, 20000, "bomb_error"), (10000, 10000, "bomb_warning"), (9000, 720, "wide")):
    path = folder / f"{tag}.png"
    bomb_png(path, width, height)
    status, text = recv([path], folder / "o")
    check(f"huge picture {width}x{height}", status == 1, text.strip()[-90:])

# A good page in a folder of other pictures: written, the others noted, exit 0.
folder = fresh()
picture(folder, "good", body, b"DAT:GOOD")
Image.new("L", (1280, 720), 40).save(folder / "holiday.png")
(folder / "notes.txt").write_text("not a picture, skipped by suffix")
status, text = recv([folder], folder / "o")
check("folder with another picture", status == 0 and (folder / "o" / "GOOD.dat").read_bytes() == body and "holiday.png: skipped" in text, text.strip().split("\n")[0][-60:])
# A broken picture among good ones may have been a page, so it does count.
(folder / "cut.jpg").write_bytes(b"\xff\xd8\xff\xe0" + bytes(50))
Image.new("RGB", (1280, 720), (9, 9, 9)).save(folder / "whole.jpg", quality=90)
data = (folder / "whole.jpg").read_bytes()
(folder / "cut.jpg").write_bytes(data[: len(data) // 2])
(folder / "whole.jpg").unlink()
status, text = recv([folder], folder / "o4")
check("cut picture among good ones", status == 1 and (folder / "o4" / "GOOD.dat").read_bytes() == body, text.strip().split("\n")[0][-60:])

# An output path that cannot be made.
folder = fresh()
pic = picture(folder, "p", body, b"DAT:X")
(folder / "file").write_text("")
status, text = recv([pic], folder / "file" / "out")
check("output folder cannot be made", status == 1, text.strip()[-60:])
status, text = recv([pic], folder / "file")
check("output is a file", status == 1, text.strip()[-60:])

# Text with half a surrogate pair: its CRC is right, so kept as sent.
folder = fresh()
lone = "A".encode("utf-16-le") + b"\x00\xd8" + "BC".encode("utf-16-le")
pic = picture(folder, "p", lone[:8], b"TXT:LONE", size=8)
status, text = recv([pic], folder / "out")
check("lone surrogate", status == 0 and (folder / "out" / "LONE.txt.utf16le").read_bytes() == lone[:8], text.strip()[-50:])

# Header fields out of range never get as far as the file.
folder = fresh()
for tag, kwargs in (("zero_pages", dict(pages=0)), ("number_high", dict(pages=2, number=2)), ("flag", dict(compressed=7))):
    pic = picture(folder, tag, body, b"DAT:H", **kwargs)
    status, text = recv([pic], folder / "o")
    check(f"bad header: {tag}", status == 1 and "damaged header" in text)

print("failures:", failures)
sys.exit(1 if failures else 0)
