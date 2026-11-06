# P4SENDX

SmileBASIC 4 program that receives files from SB4 Send, and sends files back as screenshots. It changes
P4SEND 1.2.2 by [Rei Software](http://rei.to/petit4send.html). P4SENDX is under the [MIT License](LICENSE).
[NOTICE](NOTICE) has Rei Software's permission and the list of changes.

To install it, see the [guide](../docs/guide.md#set-up).

## Menu

| Item | Use |
|---|---|
| `DETECT SYNC KEY` | Find the sync key for Fast on `Classic`. `[A] USB RECEIVE` goes on from here |
| `USB RECEIVE` | Receive a file or an archive, told apart by itself |
| `SCREENSHOT SEND` | Show a file as pages for `p4recv.py` |
| `EXIT` | End |

## Reading the screen

| Part | Meaning |
|---|---|
| Header | `SYNC KEY n`, `SLOW / 24 MS` or `NS2 / 24 MS`, from what arrives |
| Bars, top right | [Chance the next part arrives](../docs/guide.md#signal-icon) |
| `PARTS` | One cell per part: grey to come, green filling and done, orange failed |
| Long bar | This part or file. While writing, the files done |
| `RECEIVED`, `ALL PARTS` | Estimates from part size and count, exact once the last part arrives |

Saved parts start green. Failures are forgotten between runs. Past 180 parts, several share a cell, and it
turns orange if any failed.

## Run it from another project

P4SENDX can stay in one project. From direct mode in the target project:

    SUBRUN "PETIT4SEND/P4SENDX.PRG"

Files go to that project. `EXIT` ends it.

## Recommended trasfer settings
### P4SENDX
- Send Mode: Petit4Send
- P4S Compress: LZSS
- P4S Link: NS1 (**NS2 in Switch 2** for faster download)
- P4S Parity: 1 in 32 (or 1 in 16 when connection is unreliable)
- P4S Interval: 24ms (or longer when connection is unreliable)

### Original Petit4send
- Send Mode: Petit4Send
- P4S Compress: LZSS
- P4S Link: Classic
- P4S Parity: Off
- P4S Interval 30ms (or longer)

### SB4 Edit
- Send Mode: Type Text
- Text Key Delay: 70ms (or longer)
- Text CapsLock: ON