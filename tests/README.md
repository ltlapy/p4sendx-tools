# Tests

    sh tests/run.sh [WORK_DIR]

Needs a C compiler, Python 3 and Pillow. No Flipper and no Switch. Each test empties its own folder
under WORK_DIR, and stops rather than empty a folder it does not know.

| Test | What it checks |
|---|---|
| `host --unit` | The Flipper core built for a PC: names, archive headers, time left, indentation |
| `test_wire.py` | Every packet of Classic, parity and NS streams, against `p4lib.py`, written apart from it. The waits of whole batches on a made-up clock |
| `test_pack.py` | `p4pack.py`: names, conversions, skipped files, the archive's bytes against `p4lib.py` |
| `test_order.py` | `p4recv.py`: page order from the headers, other pictures in the folder |
| `test_robust.py` | `p4recv.py`: bad input |
| `test_jpeg.py` | `p4recv.py`: JPEG qualities that still read exactly |

`P4SENDX.PRG` runs only on SmileBASIC 4, so it is not tested here.
