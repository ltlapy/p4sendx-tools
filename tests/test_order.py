#!/usr/bin/env python3
"""Page order comes from the headers, not from names or dates, and a pictures
folder may hold other screenshots.

    test_order.py WORK_DIR
"""
import os
import random
import shutil
import sys
from pathlib import Path

from PIL import Image

from recvlib import encode_page, recv
from p4recv import PAYLOAD_BYTES, crc16

WORK = Path(sys.argv[1])
__import__("workdir").guard(WORK); shutil.rmtree(WORK, ignore_errors=True)
WORK.mkdir(parents=True)
rng = random.Random(21)
failures = 0


def check(case, condition, detail=""):
    global failures
    failures += not condition
    print(f"{'OK  ' if condition else 'FAIL'} {case} {detail}", flush=True)


body = rng.randbytes(PAYLOAD_BYTES * 4 + 400)
crc = crc16(body)
pages = [body[i : i + PAYLOAD_BYTES] for i in range(0, len(body), PAYLOAD_BYTES)]
count = len(pages)
order = list(range(count))
rng.shuffle(order)
assert order != sorted(order)
folder = WORK / "shots"
folder.mkdir()
for position, number in enumerate(order):
    path = folder / f"shot_{position}.png"
    encode_page(pages[number], b"DAT:SHUFFLED", len(body), crc, count, number, 0x31, 0).save(path)
    # Dates in yet another order.
    stamp = 1_700_000_000 + rng.randrange(1_000_000)
    os.utime(path, (stamp, stamp))
status, text = recv([folder], WORK / "out")
got = (WORK / "out" / "SHUFFLED.dat").read_bytes() if status == 0 else b""
check("shuffled names and dates", status == 0 and got == body, f"order {order}, {len(got)} bytes")

# The same, listed one by one in a chosen wrong order, with unrelated screenshots.
Image.new("RGB", (1280, 720), (200, 40, 10)).save(folder / "game.png")
Image.effect_noise((1280, 720), 40).save(folder / "noise.jpg", quality=85)
listed = sorted(folder.iterdir(), key=lambda p: rng.random())
status, text = recv(listed, WORK / "out2")
got = (WORK / "out2" / "SHUFFLED.dat").read_bytes() if status == 0 else b""
check("with other screenshots", status == 0 and got == body and text.count("skipped") == 2, f"exit {status}")

# Only other screenshots: nothing was found.
only = WORK / "only"
only.mkdir()
Image.new("RGB", (1280, 720), (1, 2, 3)).save(only / "game.png")
status, text = recv([only], WORK / "out3")
check("no P4SEND picture at all", status == 1 and "no P4SEND pictures" in text, f"exit {status}")

# A missing page of a real file still fails, with a screenshot beside it.
(folder / "shot_0.png").unlink()
status, text = recv([folder], WORK / "out4")
check("a page missing", status == 1 and "missing" in text, f"exit {status}")

print("failures:", failures)
sys.exit(1 if failures else 0)
