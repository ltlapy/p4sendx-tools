#!/usr/bin/env python3
"""JPEG round trips of full pages: which qualities still give the exact file.

    test_jpeg.py WORK_DIR REAL_SHOT... [--pages N]

Each real P4SEND picture (PNG) is saved as JPEG by Pillow and read back with
p4recv.py. Besides, N pages of other random data are drawn by the Python copy of
the drawing that test_robust.py checks against a real picture.
"""
import random
import shutil
import sys
from pathlib import Path

from PIL import Image

from recvlib import encode_page, recv
from p4recv import PAYLOAD_BYTES, crc16, parse_page, read_levels

WORK = Path(sys.argv[1])
__import__("workdir").guard(WORK); shutil.rmtree(WORK, ignore_errors=True)
WORK.mkdir(parents=True)
rng = random.Random(21)
real = [Path(p) for p in sys.argv[2:]]
pages = list(real)
for i in range(4):
    payload = rng.randbytes(PAYLOAD_BYTES)
    path = WORK / f"synthetic{i}.png"
    encode_page(payload, f"DAT:S{i}".encode(), len(payload), crc16(payload), 1, 0, rng.randrange(256), 0).save(path)
    pages.append(path)

LIMITED = bytes(round(16 + v * 219 / 255) for v in range(256))
QUALITIES = (100, 95, 93, 92, 91, 90, 89, 88, 87, 85, 80, 75)
failures = 0
print(f"{len(pages)} full pages ({len(real)} real), {PAYLOAD_BYTES} payload bytes each")
print("range   quality  subsampling  pages-exact  misread pixels of 518400, mean and worst")
for range_name, table in (("full", None), ("limited", LIMITED)):
    for quality in QUALITIES:
        for subsampling in ((0, 2) if quality in (100, 95, 90) else (2,)):
            exact = errors = worst = 0
            for path in pages:
                truth = read_levels(path)
                image = Image.open(path).convert("RGB")
                if table:
                    image = image.point(list(table) * 3)
                jpeg = WORK / "t.jpg"
                image.save(jpeg, quality=quality, subsampling=subsampling)
                got = read_levels(jpeg)
                wrong = sum(x != y for x, y in zip(truth, got))
                errors += wrong
                worst = max(worst, wrong)
                if parse_page(path).pages == 1:
                    # Whole files go through the command too, CRC included.
                    out = WORK / "o"
                    shutil.rmtree(out, ignore_errors=True)
                    status, text = recv([jpeg], out)
                    assert (status == 0) == (wrong == 0), text
                exact += wrong == 0
            if quality >= 91 and exact != len(pages):
                failures += 1
                print("FAIL", range_name, quality, subsampling)
            print(
                f"{range_name:8}{quality:>6}   4:{'4:4' if subsampling == 0 else '2:0'}"
                f"{exact:>11}/{len(pages)}{errors / len(pages):>14.1f}{worst:>10}",
                flush=True,
            )

# Y plane against a conversion through RGB.
path = pages[0]
image = Image.open(path).convert("RGB")
jpeg = WORK / "t.jpg"
image.save(jpeg, quality=92)
plane = Image.open(jpeg)
plane.draft("L", None)
print("draft gives mode", plane.mode, "size", plane.size)
y_plane = plane.convert("L").tobytes()
through_rgb = Image.open(jpeg).convert("RGB").convert("L").tobytes()
direct = sum(a != b for a, b in zip(y_plane, through_rgb))
print(f"Y plane and through-RGB differ in {direct} of {len(y_plane)} values, at most by",
      max(abs(a - b) for a, b in zip(y_plane, through_rgb)))

print(f"failures: {failures}")
sys.exit(1 if failures else 0)
