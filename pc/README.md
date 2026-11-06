# PC tools

Two Python 3 scripts. Pictures need [Pillow](https://python-pillow.org): `pip install Pillow`.

| Script | Use |
|---|---|
| [`p4pack.py`](#p4packpy) | Pack files or a project into an archive for SB4 Send |
| [`p4recv.py`](#p4recvpy) | Get files out of `SCREENSHOT SEND` pictures |

## p4pack.py

    python3 p4pack.py -o OUT.p4a [--part-size BYTES] [--parity 16|32|64|off] [--no-compress] INPUT...

An input is a file or a folder. A folder is a project, and only the files directly in it count.

| Input | Becomes |
|---|---|
| `.txt .prg .bas` | `TXT:`, from UTF-8 |
| `.png .jpg .jpeg .bmp`, up to 2048x2048 | `GRP:` |
| Anything else | `DAT:`, the file's bytes as 32-bit integers |
| SmileBASIC 4's own `TNAME`, `BNAME`, `GNAME` | `TXT:NAME`, `DAT:NAME`, `GRP:NAME` |

It upper-cases names, and replaces a character SmileBASIC 4 refuses with `_`. Two files with one name are an
error. It prints each entry and the expected time on each link, and warns about any file it skips. See the
[limits](../docs/guide.md#limits).

## p4recv.py

    python3 p4recv.py -o OUT_DIR [--raw] PICTURE_OR_FOLDER...

Give pictures in any order. It skips pictures with no data, and checks each file's CRC. It never replaces a
file in `OUT_DIR`: a different file of that name becomes `NAME~2`. It exits with 1 if it finds no page, or
cannot verify a file.

| Type | Output |
|---|---|
| `TXT:NAME` | `NAME.txt`, UTF-8 |
| `GRP:NAME` | `NAME.png` |
| `DAT:NAME` | `NAME.dat`, little-endian 32-bit integers |
| `--raw` | The bytes as sent, as `NAME.txt.raw` and so on |

It reads pictures from P4SENDX and from stock P4SEND.

## Licence

MIT, see [LICENSE](LICENSE). `p4recv.py` takes its brightness limits and the idea of reading JPEG's Y plane
from [p4s](https://github.com/ysh86/p4s), see [LICENSE-p4s](LICENSE-p4s).
