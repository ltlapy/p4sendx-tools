#!/usr/bin/env python3
"""The Flipper's core against the protocol, written apart from it in p4lib.py: every packet
of Classic, parity and NS streams. Then the waits of whole batches on a made-up clock.

    test_wire.py HOST WORK_DIR
"""
import glob
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

from p4lib import P4PACK, PC, host_multi, multi_packets, packets, parity_packets, read_stream, stream_of

sys.path.insert(0, str(PC))
import p4pack as P  # noqa: E402

HOST, WORK = sys.argv[1], Path(sys.argv[2])
__import__("workdir").guard(WORK)
shutil.rmtree(WORK, ignore_errors=True)
WORK.mkdir(parents=True)
rng = random.Random(14)
failures = 0
APP = Path(__file__).resolve().parent.parent / "flipper" / "sb4_send"


def check(case, ok, detail=""):
    global failures
    failures += not ok
    print(f"{'OK  ' if ok else 'FAIL'} {case} {detail}", flush=True)


def rows_of(*args):
    r = subprocess.run([HOST, *map(str, args)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    rows = []
    for line in r.stdout.split("\n"):
        if line.startswith("DATA "):
            at, mk, mouse, *keys = (int(v) for v in line[5:].split(","))
            rows.append((mk, mouse, sorted(k for k in keys if k)))
    return rows


def norm(rows):
    return [(mk, mouse, sorted(keys)) for mk, mouse, keys in rows]


def keys_only(rows):
    return [(mk, sorted(keys)) for mk, _, keys in rows]


single = WORK / "single.bin"
single_data = rng.randbytes(1500)
single.write_bytes(single_data)
AUTO, NONE = 0, 2  # as Petit4Type and Petit4Comp have them

check(
    "Classic, one file: every packet",
    rows_of(single, AUTO, NONE, -1, 0) == norm(packets(stream_of("DAT:SINGLE.BIN", single_data))),
)
for group in (16, 32, 64):
    check(
        f"parity {group}, one file: every packet",
        rows_of(single, AUTO, NONE, -1, 0, group) == norm(parity_packets("DAT:SINGLE.BIN", single_data, group)),
    )

folder = WORK / "in"
folder.mkdir()
(folder / "data.bin").write_bytes(rng.randbytes(3000))
(folder / "note.txt").write_text("HELLO\n" * 40, encoding="utf-8", newline="")


def archive(parity):
    out = WORK / f"a{parity}.p4a"
    subprocess.run([sys.executable, str(P4PACK), "-o", out, "--part-size", "1024", "--parity", str(parity), folder],
                   check=True, capture_output=True)
    return out


for group in (16, 32, 64):
    a = archive(group)
    parts = read_stream(a.read_bytes())[0]
    same = counted = True
    for index, part in enumerate(parts):
        real = rows_of("--part", a, index, 0, -1, 0)
        same = same and real == norm(parity_packets(part.name, part.data, group))
        counted = counted and len(real) == P.stream_packets(len(part.data), group)
    check(f"parity {group}, archive: every packet", same, f"{len(parts)} parts")
    check(f"parity {group}, archive: p4pack counts them", counted)
    for boards in (6, 7):
        same = True
        for index, part in enumerate(parts):
            real = host_multi(HOST, "--part", a, index, 0, -1, 0, boards=boards)
            same = same and keys_only(real) == keys_only(multi_packets(part.name, part.data, group, boards=boards))
        check(f"{boards} keyboards, parity {group}, archive: every packet", same)

for boards in (6, 7):
    real = host_multi(HOST, single, AUTO, NONE, -1, 0, 32, boards=boards)
    model = multi_packets("DAT:SINGLE.BIN", single_data, 32, boards=boards)
    check(f"{boards} keyboards, one file: every packet", keys_only(real) == keys_only(model))
    off = host_multi(HOST, single, AUTO, NONE, -1, 0, 0, boards=boards)
    check(f"{boards} keyboards: parity off still has it", off == real)

for name, a in (("no parity", archive("off")), ("parity", archive(32))):
    for wait, jitter, seed in ((5, 0, 6), (5, 8, 7), (10, 3, 2), (30, 5, 3)):
        r = subprocess.run([HOST, "--sim", a, str(wait), str(jitter), str(seed)], capture_output=True, text=True)
        check(f"batch waits, {name}, {wait} s, jitter {jitter}", r.returncode == 0, r.stdout.strip()[:90] + r.stderr.strip())

# Sounds: notes of C D E G A only, none of the firmware's own sequences.
code = "".join(open(g, encoding="utf-8").read() for g in glob.glob(str(APP / "**" / "*.[ch]"), recursive=True))
notes = re.findall(r"message_note_(\w+)", code)
check("sounds: pentatonic", notes and all(n[0] in "cdega" and n[1:].isdigit() for n in notes), sorted(set(notes)))
check("sounds: no stock sequence", not re.findall(r"&(sequence_\w+)", code))

print(f"failures: {failures}")
sys.exit(1 if failures else 0)
