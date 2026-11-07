# Changelog

## v1.1.0

P4SENDX:

- Takes parts in any order. A failed part no longer blocks the parts after it.
- After the last part, lists the missing ones, runs as `a-b`: `MISSING PARTS: 1-3, 7`.
- Link quality counts only time inside streams. After a break, a new stream replaces the old reading
  once it has run 10 s.

SB4 Send:

- `Send Part`: on an archive's Run screen, press Right or hold OK, enter a part number, and only that
  part goes. `P4S Start Part` stays as it was.

## v1.0.0

First release.
