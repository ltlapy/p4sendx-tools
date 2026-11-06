#!/usr/bin/env python3
"""p4pack.py alone: names, conversions, skipped files, and the archive's bytes.

    test_pack.py WORK_DIR
"""
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

from PIL import Image

from p4lib import P4PACK, PC, crc16, pack_image, packets, parity_packets, read_p4a2, read_stream, stream_compress, stream_expand, stream_of

sys.path.insert(0, str(PC))
import p4pack  # noqa: E402

WORK = Path(sys.argv[1])
__import__("workdir").guard(WORK); shutil.rmtree(WORK, ignore_errors=True)
WORK.mkdir(parents=True)
failures = 0


def check(case, condition, detail=""):
    global failures
    failures += not condition
    print(f"{'OK  ' if condition else 'FAIL'} {case} {detail}", flush=True)


def pack(out, *inputs, part_size=None, no_compress=False):
    command = [sys.executable, str(P4PACK), "-o", str(out)]
    if no_compress:
        command += ["--no-compress"]
    if part_size is not None:
        command += ["--part-size", str(part_size)]
    r = subprocess.run(command + [str(i) for i in inputs], capture_output=True, text=True)
    assert "Traceback" not in r.stderr, r.stderr
    return r.returncode, r.stdout, r.stderr


def read_archive(path):
    """The image of a .p4a, every row and part checked by the tests' own reader."""
    _, data = read_p4a2(path.read_bytes())
    part_size = struct.unpack_from("<I", path.read_bytes(), 8)[0]
    count = struct.unpack_from("<I", data)[0]
    at = 4
    entries = {}
    for _ in range(count):
        name = data[at : at + 36].rstrip(b"\0").decode()
        size, crc, zero = struct.unpack_from("<IHH", data, at + 36)
        body = data[at + 44 : at + 44 + size]
        assert crc == crc16(body) and zero == 0, name
        entries[name] = body
        at += 44 + size + (-size % 4)
    assert at == len(data), "trailing bytes"
    return entries, part_size, data


def fresh(name):
    folder = WORK / name
    folder.mkdir()
    return folder


def container(kind, body, usage=0, compress=False):
    """SmileBASIC 4's header, with a footer that nobody reads: no key is at hand here."""
    stored = zlib.compress(body) if compress else body
    head = bytearray(0x70)
    struct.pack_into("<HHHHI", head, 0, 4, kind, 1 if compress else 0, usage, len(stored))
    struct.pack_into("<HBBBBB", head, 12, 2026, 11, 3, 1, 2, 3)
    head[0x13] = 0xFF
    return bytes(head) + stored + hashlib.sha1(stored).digest()


def pcbn(element, sizes, data):
    all_sizes = list(sizes) + [0] * (4 - len(sizes))
    return b"PCBN0004" + struct.pack("<HH4I", element, len(sizes), *all_sizes) + data


# PC files: types, names.
folder = fresh("pc")
(folder / "hello.txt").write_bytes("﻿안녕 HI\n".encode("utf-8"))
(folder / "main.prg").write_bytes(b'?"HI"\n')
(folder / "old.bas").write_bytes(b"10 ?1\n")
(folder / "map.bin").write_bytes(bytes(range(8)))
(folder / "raw.dat").write_bytes(bytes(range(6)))
(folder / "weird").write_bytes(b"\1\2\3\4")
(folder / "PHOTO.JPG").write_bytes(b"")
Image.new("RGB", (3, 2), (10, 20, 30)).save(folder / "PHOTO.JPG", quality=100)
image = Image.new("RGBA", (3, 2))
image.putdata([(x * 80, y * 90, 7, 200 + x) for y in range(2) for x in range(3)])
image.save(folder / "pic.png")
Image.new("RGB", (2, 1), (1, 2, 3)).save(folder / "b.bmp")
out = WORK / "pc.p4a"
status, text, errors = pack(out, folder)
entries, part_size, _ = read_archive(out)
check("exit 0, default part size", status == 0 and part_size == 16384 and "at your own risk" not in errors, f"{part_size}")
names = sorted(entries)
want = ["DAT:MAP.BIN", "DAT:RAW", "DAT:WEIRD", "GRP:B", "GRP:PHOTO", "GRP:PIC", "TXT:HELLO", "TXT:MAIN.PRG", "TXT:OLD"]
check("names", names == want, str(names))
check("TXT, BOM dropped, UTF-16LE", entries["TXT:HELLO"] == "안녕 HI\n".encode("utf-16-le"))
check("DAT bytes as they are", entries["DAT:MAP.BIN"] == bytes(range(8)))
check("short DAT keeps its size and is noted", entries["DAT:RAW"] == bytes(range(6)) and "ends with zeros" in text)
pixels = [struct.pack("<HH", 3, 2)]
for y in range(2):
    for x in range(3):
        pixels.append(bytes((7, y * 90, x * 80, 200 + x)))
check("GRP is width, height, BGRA", entries["GRP:PIC"] == b"".join(pixels))
check("GRP from JPEG has alpha 255", entries["GRP:PHOTO"][7] == 255 and entries["GRP:PHOTO"][:4] == struct.pack("<HH", 3, 2))
check("GRP from BMP", entries["GRP:B"] == struct.pack("<HH", 2, 1) + bytes((3, 2, 1, 255)) * 2)
check("prints", "9 entries" in text and "1 part(s)" in text and "at Slow:" in text and "at Fast:" in text, text.split("\n")[-4])

# Time: exact, from the packets of the very streams.
rows = read_p4a2(out.read_bytes())[0]
count = sum(len(parity_packets(p.name, p.data, 32)) for p in rows)
slow = count * 24 / 1000 + 10 * len(rows)
fast = count / 60 + 10 * len(rows)
check(
    "time from the real packet count",
    f"{count} packets" in text and f"at Slow: {p4pack.duration(slow)}" in text and f"at Fast: {p4pack.duration(fast)}" in text,
    f"{count} packets",
)
check("no words about before compression in the time", "before any" not in text and "bytes before compression" in text)
wrong = [n for n in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 37, 38, 100, 65548) if len(packets(stream_of("DAT:X", bytes(n)))) != p4pack.stream_packets(n)]
check("stream_packets agrees with the packets", not wrong, str(wrong))
wrong = [
    (n, g)
    for n in (0, 4, 8, 100, 148, 149, 152, 153, 1036, 16396, 65548)
    for g in (16, 32, 64)
    if len(parity_packets("DAT:X", bytes(n), g)) != p4pack.stream_packets(n, g)
]
check("with parity too", not wrong, str(wrong))

# Compression: exact against a slow proof, and each format's edges.
random_bytes = __import__("random").Random(8).randbytes


def brute_bits(data):
    """Every token at every position, against which the parse's shortcut is checked."""
    cost = [0] * (len(data) + 1)
    for at in range(len(data) - 1, -1, -1):
        best = cost[at + 1] + 9
        for start in range(max(0, at - 65536), at):
            n = 0
            while n < 258 and at + n < len(data) and data[start + n] == data[at + n]:
                n += 1
                if n >= 3:
                    best = min(best, cost[at + n] + 26)
                if n >= 2 and n <= 65 and at - start <= 1024:
                    best = min(best, cost[at + n] + 18)
        cost[at] = best
    return cost[0]


samples = {
    "text": b"the rain in spain falls mainly on the plain " * 12,
    "runs": bytes(100) + b"\1" * 70 + bytes(90) + b"ab" * 60,
    "noise": random_bytes(400),
    "mixed": (random_bytes(30) + bytes(9)) * 14,
    "short": b"abcabc",
    "one": b"x",
    "far": random_bytes(1100) + b"0123456789" + random_bytes(200) + b"0123456789",
    "pairs": random_bytes(1300) + b"zq" + random_bytes(30) + b"zq" + b"zq",
}
for name, data in samples.items():
    packed = p4pack.lz_compress(data)
    whole = (brute_bits(data) + 7) // 8
    # Flag bits are whole bytes, and bits unused in the last flag byte are nothing more.
    check(f"optimal parse, {name}", stream_expand(packed, len(data)) == data and len(packed) * 8 - brute_bits(data) in range(0, 8), f"{len(packed)} of {len(data)}, bound {whole}")
for name, data in (("text", samples["text"] * 8), ("mixed", samples["mixed"] * 3)):
    check(f"not worse than greedy, {name}", len(p4pack.lz_compress(data)) <= len(stream_compress(data)))
limits = bytes(70000) + random_bytes(65530) + bytes(40) + random_bytes(66000)
check("a run, past 64 KiB", stream_expand(p4pack.lz_compress(limits), len(limits)) == limits)

# The stream: compressed as a whole; a part is its bytes as they are.
modes_folder = fresh("modes")
far = random_bytes(1500)
(modes_folder / "a.bin").write_bytes(random_bytes(4000))
(modes_folder / "b.bin").write_bytes(b"ab" * 2000)
(modes_folder / "c.bin").write_bytes((far + far)[:3000] + bytes(-3000 % 4))
out3 = WORK / "modes.p4a"
status, text3, _ = pack(out3, modes_folder, part_size=1024)
rows, stream3 = read_stream(out3.read_bytes())
image3 = read_p4a2(out3.read_bytes())[1]
check("stream head", stream3[:4] == b"P4A3" and struct.unpack_from("<I", stream3, 4)[0] == zlib.crc32(stream3[8:]) and struct.unpack_from("<II", stream3, 8) == (1, len(image3)))
check("the whole archive compressed, then split", len(stream3) == 16 + len(p4pack.lz_compress(image3)) + (-len(p4pack.lz_compress(image3)) % 4) and len(rows) == (len(stream3) + 1023) // 1024)
check("every part is plain", {p.compress for p in rows} == {0} and all(p.sent == p.data for p in rows))
check("says what it did", "compressing" in text3 and ": LZ" in text3 and f"{len(image3)} bytes before compression, {sum(len(p.sent) for p in rows)} after" in text3)
out4 = WORK / "plain.p4a"
_, text4, _ = pack(out4, modes_folder, part_size=1024, no_compress=True)
rows4, image4 = read_p4a2(out4.read_bytes())
stream4 = read_stream(out4.read_bytes())[1]
check("--no-compress: same image, method 0", image4 == image3 and struct.unpack_from("<I", stream4, 8)[0] == 0 and "not compressed" in text4 and "compressing" not in text4)
noise_folder = fresh("noise")
(noise_folder / "a.bin").write_bytes(random_bytes(3000))
out5 = WORK / "noise.p4a"
pack(out5, noise_folder)
stream5 = read_stream(out5.read_bytes())[1]
check("incompressible: method 0", struct.unpack_from("<I", stream5, 8)[0] == 0 and read_p4a2(out5.read_bytes())[1][48:] == (noise_folder / "a.bin").read_bytes())
check("no compressing per part left in the tool", "LZ_FORMATS" not in P4PACK.read_text() and "compress_part" not in P4PACK.read_text())

# Same input, same archive.
out2 = WORK / "pc2.p4a"
pack(out2, folder)
check("deterministic", out.read_bytes() == out2.read_bytes())

# Part size.
for bad in (3, 65537, 0, -4):
    status, _, errors = pack(WORK / "x.p4a", folder, part_size=bad)
    check(f"--part-size {bad} refused", status == 1 and "multiple of 4" in errors)
status, text, _ = pack(WORK / "p.p4a", folder, part_size=1024)
parts = len(read_p4a2((WORK / "p.p4a").read_bytes())[0])
check("--part-size 1024", status == 0 and f"{parts} part(s) of 1024" in text, f"{parts} parts")
status, _, errors = pack(WORK / "big.p4a", folder, part_size=16388)
check("over 16384: warned, accepted", status == 0 and "warning: the larger the part size, the longer a failure takes to recover from. Use at your own risk." in errors, errors[-120:])
status, _, errors = pack(WORK / "edge.p4a", folder, part_size=16384)
check("16384: no warning", status == 0 and "own risk" not in errors)
status, _, errors = pack(WORK / "x.p4a", folder, part_size=12)
check("--part-size under the stream's header refused", status == 1 and "under 16" in errors)

# SmileBASIC 4's own files.
folder = fresh("native")
source = "?\"안녕\"\n".encode()
(folder / "TMAIN.PRG").write_bytes(container(0, source, usage=2))
(folder / "TNOTE").write_bytes(container(0, "압축 text".encode(), compress=True))
ints = struct.pack("<3i", 1, -2, 3)
(folder / "BMAP").write_bytes(container(1, pcbn(4, [3], ints), usage=1, compress=True))
(folder / "BBARE").write_bytes(pcbn(4, [3], ints))
(folder / "BREAL").write_bytes(container(1, pcbn(5, [2], struct.pack("<2d", 1.5, 2.0)), usage=1, compress=True))
(folder / "BSTR").write_bytes(container(1, pcbn(6, [1], b"\x01A"), usage=1, compress=True))
(folder / "B2D").write_bytes(container(1, pcbn(4, [1, 3], ints), usage=1, compress=True))
grid = struct.pack("<2i", 0x01020304, 0x05060708)
(folder / "GSPR").write_bytes(container(2, pcbn(4, [2, 1], grid), usage=3, compress=True))
(folder / "GOLD").write_bytes(container(1, pcbn(4, [2, 1], grid), usage=2, compress=True))
Image.new("RGB", (1, 1), (9, 8, 7)).save(folder / "GPNG", format="PNG")
(folder / "JSHOT").write_bytes(container(5, b"\xff\xd8\xff", usage=3))
(folder / "META").write_bytes(container(4, b"PCPM0005"))
(folder / "T@BACKUP.TXT").write_bytes(container(0, b"old"))
(folder / "sub").mkdir()
(folder / ".hidden").write_bytes(b"x")
(folder / "TBAD").write_bytes(container(0, b"\xff\xfe not utf-8"))
out = WORK / "native.p4a"
status, text, errors = pack(out, folder)
entries, _, _ = read_archive(out)
want = ["DAT:BARE", "DAT:MAP", "GRP:OLD", "GRP:PNG", "GRP:SPR", "TXT:MAIN.PRG", "TXT:NOTE"]
check("native names", sorted(entries) == want, str(sorted(entries)))
check("native text", entries["TXT:MAIN.PRG"] == source.decode().encode("utf-16-le") and entries["TXT:NOTE"] == "압축 text".encode("utf-16-le"))
check("native DAT, compressed and bare", entries["DAT:MAP"] == ints and entries["DAT:BARE"] == ints)
check("native GRP", entries["GRP:SPR"] == struct.pack("<HH", 2, 1) + grid and entries["GRP:OLD"] == entries["GRP:SPR"])
check("container-less GRP PNG", entries["GRP:PNG"] == struct.pack("<HH", 1, 1) + bytes((7, 8, 9, 255)))
for name, why in [
    ("BREAL", "reals"), ("BSTR", "strings"), ("B2D", "flatten"), ("JSHOT", "JPG"),
    ("META", "METASAVE"), ("TBAD", "UTF-8"), ("T@BACKUP.TXT", "backup"), ("sub", "folder"), (".hidden", "own"),
]:
    line = [l for l in errors.split("\n") if name in l]
    check(f"skipped with a warning: {name}", len(line) == 1 and why in line[0], line[0][-70:] if line else "no warning")

# A surrogate pair apart, as SmileBASIC 4's text files hold one, and one whole.
folder = fresh("astral")
(folder / "TAPART").write_bytes(container(0, b"\xed\xa0\xb4\xed\xb4\x9e"))
(folder / "whole.txt").write_bytes("𝄞".encode("utf-8"))
out = WORK / "astral.p4a"
status, text, errors = pack(out, folder)
entries, _, _ = read_archive(out)
pair = "𝄞".encode("utf-16-le")
check("surrogates either way", entries["TXT:APART"] == pair and entries["TXT:WHOLE"] == pair and "two units" in text)

# Name rules.
folder = fresh("names")
(folder / "a b+c.txt").write_bytes(b"x")
(folder / "한글.txt").write_bytes(b"x")
(folder / ("L" * 40 + ".txt")).write_bytes(b"x")
(folder / "con.txt").write_bytes(b"x")
(folder / "x.y.z").write_bytes(bytes(4))
(folder / "keep@-_.prg").write_bytes(b"x")
out = WORK / "names.p4a"
status, text, errors = pack(out, folder)
entries, _, _ = read_archive(out)
want = sorted(["TXT:A_B_C", "TXT:__", "TXT:" + "L" * 32, "TXT:_CON", "DAT:X.Y.Z", "TXT:KEEP@-_.PRG"])
check("names made legal, with warnings", sorted(entries) == want and errors.count("would refuse") == 4, str(sorted(entries)))

# Clashes are errors, and leave no file.
folder = fresh("clash")
(folder / "same.txt").write_bytes(b"x")
(folder / "SAME.bas").write_bytes(b"y")
out = WORK / "clash.p4a"
status, _, errors = pack(out, folder)
check("clash", status == 1 and "both be TXT:SAME" in errors and not out.exists(), errors.strip())
folder = fresh("clash2")
(folder / "a+.txt").write_bytes(b"x")
(folder / "a-.txt").write_bytes(b"x")
(folder / "a_.txt").write_bytes(b"x")
(folder / "a!.txt").write_bytes(b"x")
status, _, errors = pack(WORK / "clash2.p4a", folder)
check("clash after cleaning", status == 1 and "TXT:A_" in errors)
folder = fresh("clash3")
(folder / "same.txt").write_bytes(b"x")
(folder / "same.bin").write_bytes(bytes(4))
status, _, errors = pack(WORK / "clash3.p4a", folder)
check("same stem, other name", status == 0)
folder = fresh("partname")
(folder / "P4A00001").write_bytes(bytes(4))
status, _, errors = pack(WORK / "partname.p4a", folder)
check("the name of a part refused", status == 1 and "part of an archive" in errors)

# Limits and refusals.
folder = fresh("limits")
Image.new("RGB", (2049, 1)).save(folder / "toowide.png")
Image.new("RGB", (2048, 1)).save(folder / "fits.png")
(folder / "notimage.png").write_bytes(b"no")
(folder / "bad.txt").write_bytes(b"\xff\xff")
(folder / "over.bin").write_bytes(bytes((16 << 20) + 4))
(folder / "long.txt").write_bytes(b"a" * ((1 << 20) + 1))
status, text, errors = pack(WORK / "limits.p4a", folder, part_size=1 << 22, no_compress=True)
entries, _, _ = read_archive(WORK / "limits.p4a")
check("over 2048 skipped", "toowide.png: SKIPPED" in errors and "GRP:FITS" in entries and "GRP:TOOWIDE" not in entries)
check("broken image, bad UTF-8 skipped", "notimage.png: SKIPPED" in errors and "bad.txt: SKIPPED" in errors)
check("16 MiB limit warned", "DAT:OVER.BIN" in errors and "limit" in errors)
check("program slot limit warned", "TXT:LONG" in errors and "program slot" in errors)
status, _, errors = pack(WORK / "none.p4a", fresh("empty"))
check("nothing to pack", status == 1 and "nothing" in errors)
status, _, errors = pack(WORK / "none.p4a", WORK / "not-there")
check("no such input", status == 1 and "no such" in errors)
status, _, errors = pack(WORK / "none.p4a", WORK / "pc", part_size=(16 << 20) + 4)
check("part size too large", status == 1 and "over" in errors)

# The same bytes as the tests' own archive maker.
folder = fresh("same")
(folder / "a.bin").write_bytes(bytes(range(10)))
(folder / "b.txt").write_bytes(b"xyz")
status, _, _ = pack(WORK / "same.p4a", folder, part_size=100)
mine = pack_image([("DAT:A.BIN", bytes(range(10))), ("TXT:B", "xyz".encode("utf-16-le"))])
check("image bytes", read_p4a2((WORK / "same.p4a").read_bytes())[1] == mine)

print("failures:", failures)
sys.exit(1 if failures else 0)
