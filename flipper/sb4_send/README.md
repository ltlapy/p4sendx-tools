# SB4 Send

Flipper Zero app that sends files from the SD card to SmileBASIC 4 over USB. Built for Momentum firmware
mntm-012, API 87.1.

- [Use it](#use-it)
- [Config](#config)
- [Build](#build)
- [Licence and changes](#licence-and-changes)

## Use it

Copy the release's `.fap` for your firmware to `apps/USB/` on the SD card, open `SB4 Send`, and
pick a file. The [guide](../../docs/guide.md) covers full transfers.

| Button | Run screen |
|---|---|
| OK | `Run`, `Stop`, `End`, `Cancel` |
| Hold OK | `Send Part`, on an archive, when idle |
| Left | `Config`, when idle |
| Right | `Send Part`, on an archive, when idle. `Pause` and `Resume`, while sending |
| Up, Down | `P4S Slow`, `P4S Detect`, `P4S Key 0` to `24`, on `Classic` |

`Send Part` asks for a part number and sends that part alone, for a part SB4 is missing. The screen
says `Part k only`, and the time left is for that part. `P4S Start Part` stays as it was.

The bottom left shows the time left. The second line shows the time so far.

## Config

The list depends on the file. The first 4 bytes tell an archive, not its name.

| File | Items |
|---|---|
| Archive | `P4S Link`, `P4S Interval`, `P4S Start Part`, `P4S Part Wait` |
| Other, `Petit4Send` | `Send Mode`, `P4S File Type`, `P4S Compress`, `P4S Link`, `P4S Parity`, `P4S Interval` |
| Other, `Type Text` | `Send Mode`, `Text Key Delay`, `Text CapsLock`, `Keyboard Layout` |

| Item | Values | Default |
|---|---|---|
| `Send Mode` | `Type Text`, `Petit4Send` | `Type Text` |
| `P4S File Type` | `Auto`, `TXT`, `DAT`, `GRP` | `Auto` |
| `P4S Compress` | `Auto`, `LZSS`, `None` | `Auto` |
| `P4S Link` | `Classic`, `NS1`, `NS2` | `Classic` |
| `P4S Parity` | `Off`, `1 in 16`, `1 in 32`, `1 in 64` | `Off` |
| `P4S Interval` | 24 to 100 ms | 24 ms |
| `P4S Start Part` | Number entry | 0 |
| `P4S Part Wait` | 5 to 120 s | 10 s |
| `Text Key Delay` | 30 to 300 ms | 70 ms |
| `Text CapsLock` | `ON`, `OFF` | `ON` |
| `Keyboard Layout` | File browser | Firmware's default |

The app keeps its settings in `badusb/.sb4send.settings` on the SD card.

## Build

For official firmware 1.4.x:

    ufbt update --hw-target f7 --url https://update.flipperzero.one/builds/firmware/1.4.3/flipper-z-f7-sdk-1.4.3.zip
    ufbt

For 1.5.x, put `1.5.1` in the URL. For Momentum, use
`https://up.momentum-fw.dev/builds/firmware/mntm-012/flipper-z-f7-sdk-mntm-012.zip`. Momentum has the
same API number as 1.4.x, but some structs differ, so build for it with its own SDK. The result is
`dist/sb4_send.fap`.
`helpers/petit4send.c` has no Flipper dependencies, so [`tests/run.sh`](../../tests) runs it on a PC.

## Licence and changes

GPLv3, see [LICENSE](LICENSE).

**This is a modified version** of the Bad KB app (`applications/main/bad_usb`) of
[Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware), itself from the Flipper Zero
firmware, both GPLv3. ltlapy took it at release mntm-012 and changed it from November 2026:

- Added: Type Text, Petit4Send sending, archives in parts, parity, the NS1 and NS2 links, sync
  keys, time left, its own USB devices, settings file and sounds.
- Removed: BadUSB scripts and BLE, with their Config items and settings, and the USB name and VID/PID
  settings.
- Renamed: `helpers/ducky_script*` to `helpers/send_worker*`, and script names in the code. Names
  beginning `bad_usb_` come from upstream.
- Replaced: `icon.png`.
- Copied: the firmware icons the app uses, into `images/`, from official firmware 1.5.1, so the official
  SDK builds it.
- Kept: the app's folder `badusb` on the SD card, which also holds the firmware's keyboard layouts.

Upstream's source files have no licence headers of their own, so none were added.
