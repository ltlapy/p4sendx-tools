# Guide

1. [Set up](#set-up)
2. [Send a file](#send-a-file)
3. [Send a project](#send-a-project)
4. [Pick a speed](#pick-a-speed)
5. [When a transfer fails](#when-a-transfer-fails)
6. [Get files back](#get-files-back)
7. [Reference](#reference)
8. [Hardware results](#hardware-results)

## Set up

### Flipper

Pick the `.fap` for your firmware from a release; the release notes list them. Copy it to `apps/USB/`
on the SD card. It appears as `SB4 Send`.

If the Flipper reports an API mismatch, [build it](../flipper/sb4_send/README.md#build) for your
firmware.

### SmileBASIC 4

Copy `p4sendx/P4SENDX.PRG` to the SD card and send it once. Stock P4SEND takes about 3 min 30 s;
typing takes about 72 min and needs no other program. Both times are calculated.

**With stock P4SEND:**

1. Install P4SEND with public key `4BKXWKAE`, from [Rei Software's page](http://rei.to/petit4send.html).
2. Make a project called `TOOLS` and make it current. Run P4SEND and choose `USB RECEIVE`.
3. In SB4 Send, pick `P4SENDX.PRG`. Open `Config` and set `Send Mode` to `Petit4Send`, `P4S Link` to
   `Classic` and `P4S Parity` to `Off`.
4. Press `Run` at `P4S Slow`. Press A when P4SEND asks to save `TXT:P4SENDX.PRG`.

**By typing:**

1. Open an empty program in SmileBASIC 4's editor.
2. In SB4 Send, pick `P4SENDX.PRG`. Open `Config` and set `Send Mode` to `Type Text`, `Text Key Delay`
   to 70 ms and `Text CapsLock` to `ON`.
3. Press `Run`. Save the program as `P4SENDX.PRG` when it ends. If keys go missing, raise the delay.

### Switch

Set Auto-Sleep to Never, in System Settings > Sleep Mode. A program cannot prevent sleep, and sleep
stops a transfer.

Leave the mouse and controller alone during a transfer.

## Send a file

1. On SmileBASIC 4, make the target project current, run P4SENDX and choose `USB RECEIVE`. Press A or
   Enter at `TURN OFF AUTO-SLEEP`.
2. In SB4 Send, pick any file. Open `Config` and set `Send Mode` to `Petit4Send`.
3. Press `Run`. Answer the save prompt when the file arrives.

To protect the file against lost packets, set `P4S Parity` to `1 in 32`. Parity needs P4SENDX.

**File types.** `.txt`, `.prg` and `.bas` go as `TXT:`, `.bmp` as `GRP:`, and the rest as `DAT:`. Change
this with `P4S File Type`. A BMP must be uncompressed, 24 or 32-bit, up to 2048x2048. For other image
formats, use an archive. A `GRP:` file appears on screen first: press any button to reach the prompt.

**File names.** SB4 Send drops only `.txt .dat .bas .bmp .png .jpg .jpeg`, since SmileBASIC 4 uses a
file's whole name:

| On the card | On SmileBASIC 4 |
|---|---|
| `hello.txt` | `TXT:HELLO` |
| `main.prg` | `TXT:MAIN.PRG` |
| `map.bin` | `DAT:MAP.BIN` |

### Type into the editor

Set `Send Mode` to `Type Text` to type a file into whatever has focus. SB4 Send keeps the file's
indentation in the editor, which repeats each line's indent by itself. A tab at the start of a line
counts as one space. It skips anything beyond ASCII. It taps CapsLock at the start and again at the end,
or when you stop.

## Send a project

An archive holds many files, compressed on the PC.

1. Pack a folder or a list of files. A project folder copied from SmileBASIC 4 also works:

       python3 pc/p4pack.py -o my.p4a MY_PROJECT_FOLDER
       python3 pc/p4pack.py -o my.p4a main.prg sprites.png map.bin

   It prints each file and the expected time on each link.
2. Copy `my.p4a` to the SD card.
3. Choose the target project on SmileBASIC 4. A running program can write only to its current project,
   and cannot create one. Use either way:

   - From direct mode in the target project, run P4SENDX as a subprogram:
     `SUBRUN "TOOLS/P4SENDX.PRG"`. `EXIT` ends it.
   - Or load it there: `PROJECT "TARGET"`, `LOAD "TOOLS/P4SENDX.PRG"`, `RUN`.

4. Choose `USB RECEIVE`, and press A or Enter at the warning.
5. In SB4 Send, pick `my.p4a`. Open `Config` first if you need it: leaving `Config` forgets a sync key.
6. Press `Run` and wait. The Flipper taps Enter to answer save prompts.
7. At `ALL DONE`, press `Stop` on the Flipper first, then B on the controller. On the title screen, a
   tapped Enter would choose a menu item.

`IGNORED` on the final screen counts files that match what the project already has. They bring no prompt.

## Pick a speed

Set `P4S Link` in `Config`. USB reconnects when you leave `Config`.

| Link | Use for | Reads it |
|---|---|---|
| `Classic` | Stock P4SEND | P4SEND, P4SENDX |
| `NS1` | Switch 1 | P4SENDX |
| `NS2` | Switch 2 | P4SENDX |

`NS1` shows the console 6 keyboards and `NS2` shows 7. A USB probe at 24 ms a packet gave this advice, on one
console of each kind. A Switch 2 read 7 keyboards with every packet exact. A Switch 1 read 6 with every packet
exact, and with 7, up to 14 of 625 packets had a report one frame late. If you try `NS2` on a Switch 1, watch
`LATE` and `RETRIES`.

### Slow and Fast

`Classic` has two speeds, chosen with Up and Down on the Run screen. `P4S Slow` needs no setup. Fast
sends one packet per frame, and needs a sync key, the timing offset between the Flipper and the console:

1. In P4SENDX choose `DETECT SYNC KEY`. On the Flipper choose `P4S Detect` and press `Run`.
2. After about 10 s, `SYNC KEY: n` appears. Stop the Flipper.
3. Press A in P4SENDX to go to `USB RECEIVE`. On the Flipper choose `P4S Key n` and press `Run`.

A key lasts while USB stays connected. Unplugging, sleep or opening `Config` loses it.

### Parity

Parity repairs one lost packet in each group, without resending. An archive takes it from
`p4pack.py --parity`, and a single file from `P4S Parity`. The NS links always use it. Only
P4SENDX reads parity, so keep it `Off` for stock P4SEND.

| Parity | Extra packets |
|---|---:|
| 1 in 16 | about 7% |
| 1 in 32 | about 3.4% |
| 1 in 64 | about 1.7% |

### Signal icon

The bars at the top right of the receiving screen estimate the chance that the next part arrives, from
the last 2.5 minutes of streams. Waits and idle time do not count. After a break, a new stream replaces
the old reading once it has run 10 s.

| Bars | Chance |
|---:|---:|
| 4 | 95% or more |
| 3 | 80% or more |
| 2 | 50% or more |
| 1 | 20% or more |
| Grey | Below that, or the first 20 s |

## When a transfer fails

### A part failed

P4SENDX shows `PART k FAILED`, and the part turns orange on the bar. The Flipper goes on with the next
part, and P4SENDX takes it. After the last part, P4SENDX lists the missing parts, e.g.
`MISSING PARTS: 1-3, 7`. For each one:

1. Press `Stop` on the Flipper.
2. Press Right, or hold OK. Enter the part number and press OK.

Parts count from 0. To send from a part to the end instead, set `P4S Start Part` in `Config`. It stays
saved, so **set it back to 0 for the next archive**.

### You stopped, or the console slept

Run `USB RECEIVE` again. P4SENDX finds the saved parts and lists the missing ones. Send them as above,
or set `P4S Start Part` to the first one.

`[X] UNFINISHED TRANSFERS` lists saved archives, with `RESUME` and `DELETE`. Delete asks for X on the
controller, since keys in a transfer could work a menu.

### Messages

| Message | Do this |
|---|---|
| `TRY AGAIN` | Send the file again. P4SENDX wrote nothing. |
| `CRC DOES NOT MATCH` | Send the file again, at a lower speed. `[A] SAVE ANYWAY` keeps a damaged file. |
| `DIFFERENT ARCHIVE` | Saved parts belong to another archive. Resume or delete them under `[X]`. |
| `OLD ARCHIVE`, or `Old P4A: use new p4pack` on the Flipper | Pack the archive again. |
| `n OF m FILES WRITTEN` | Run `USB RECEIVE` to retry. P4SENDX kept the parts. |
| `CANCELLED` beside a file | The Flipper stopped before answering its prompt. Retry, and stop it later. |

### Too many failures

Try these in order:

1. Raise `P4S Interval` to 30 or 40 ms.
2. Use `p4pack.py --parity 16`.
3. Use smaller parts with `--part-size`.
4. Change `NS2` to `NS1`.

A `P4S Part Wait` that is too short also fails parts: a prompt is still open when data arrives, and
SmileBASIC 4 sees a cancelled save. Raise the wait and resend the part.

### Nothing arrives on an NS link

Run P4SENDX. Stock P4SEND cannot read these links.

### Out of memory while writing

P4SENDX loads a same-named file to compare it, and cannot know its size first. A file of about 115 MB
in memory stops it: 29 million integers or 58 million characters, saved compressed by a program. Remove
that file and run `USB RECEIVE`. The parts stay saved.

## Get files back

1. In P4SENDX choose `SCREENSHOT SEND`, pick a file, and press A at `READY`.
2. Take a screenshot of each page with the capture button, and press any button for the next page.
3. Copy the screenshots to a PC and run:

       python3 pc/p4recv.py -o OUT_DIR PICTURES_OR_FOLDER

`p4recv.py` ignores order and file names, skips other screenshots, and checks each file's CRC. Tests show
it needs JPEG quality 91 or above, and it reads the Switch's own screenshots.

| Type | Output |
|---|---|
| `TXT:` | `.txt`, UTF-8 |
| `GRP:` | `.png` |
| `DAT:` | `.dat`, little-endian 32-bit integers |
| Failed CRC | `.bad` |

`--raw` writes the bytes as sent.

## Reference

### Config

An archive is recognised by its first 4 bytes, not its name.

| File | Items |
|---|---|
| Archive | `P4S Link`, `P4S Interval`, `P4S Start Part`, `P4S Part Wait` |
| Other, `Petit4Send` | `Send Mode`, `P4S File Type`, `P4S Compress`, `P4S Link`, `P4S Parity`, `P4S Interval` |
| Other, `Type Text` | `Send Mode`, `Text Key Delay`, `Text CapsLock`, `Keyboard Layout` |

### p4pack.py

| Option | Default | Meaning |
|---|---|---|
| `--part-size BYTES` | 16384 | Part size. A failed part goes again whole, so larger sizes print a warning. |
| `--parity 16\|32\|64\|off` | 32 | One packet in that many is parity. |
| `--no-compress` | | Store without compressing. |

### Final screen

| Item | Meaning |
|---|---|
| `IGNORED` | Files already the same, not written |
| `REPAIRED` | Packets rebuilt from parity |
| `LATE` | NS link packets that needed a second read |
| `RETRIES` | Parts sent again |
| `UNPACK`, `SAVE` | Seconds spent on each |

### Sounds

| Where | Sound | Meaning |
|---|---|---|
| SmileBASIC 4 | C E G C | Done |
| SmileBASIC 4 | C C C, twice | You are needed |
| Flipper | Rising | Sent |
| Flipper | Falling | Error, or USB gone |

### Limits

- The Flipper cannot hear back from the Switch, so only P4SENDX knows about a failure.
- Each written file costs a save prompt of about 2.6 to 5 s. No program can skip it. Overwriting brings
  two prompts, which the Flipper's Enter answers, and keeps the old file as `@BACKUP`.
- `p4pack.py` skips these with a warning: JPG files, `META`, DATs of reals or strings, arrays of 2 or more
  dimensions, and subfolders.
- A file's body must fit SmileBASIC 4's limit of 16 MiB, after its own compression.
- The clock cannot show four packets lost in a row. The CRC catches them.

## Hardware results

**Ran on real consoles**, reported by the one user:

- `Classic`, Slow and Fast, with `GRP:`, to stock P4SEND.
- Archives into P4SENDX on a Switch 2: 4 files, then a project of 13. The same project later failed at
  Slow with lost packets, before parity existed.
- Typing into the editor with CapsLock and indentation.
- `Config`, the screens, a `SCREENSHOT SEND` page read back by `p4recv.py`, and `SUBRUN`, with files landing
  in the main side's project.
- The USB probe above, and general use on the NS links, with no timings or counts recorded.

**Ran only in tests:** packets lost or late at chosen places, time left, packets against the protocol.

**Not checked yet:**

- A big archive with parity, on `Classic` and the NS links, with `LATE`, `REPAIRED`, `RETRIES` and
  its time.
- `UNFINISHED TRANSFERS`, resuming and deleting.
- CapsLock tapped back after stopping.
- Drawing cost per frame on a Switch 1.
- `UNPACK` against a stopwatch: the clock may not run in a long loop.
- Firmware other than mntm-012.
