#!/bin/sh
# Builds the Flipper app's core for this computer and runs every test here.
# Needs a C compiler, Python 3 and Pillow. Nothing else: no Flipper, no Switch.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$(mktemp -d)}
helpers="$here/../flipper/sb4_send/helpers"
export PYTHONDONTWRITEBYTECODE=1
cc=${CC:-cc}
mkdir -p "$work"
"$cc" -O1 -I"$helpers" -o "$work/host" "$here/host.c" "$helpers/petit4send.c"
status=0
echo "== host --unit"
"$work/host" --unit || status=1
for test in test_wire.py:"$work/host" test_pack.py test_order.py test_robust.py test_jpeg.py; do
    name=${test%%:*}
    extra=""
    [ "$name" != "$test" ] && extra=${test#*:}
    echo "== $name"
    if ! python3 "$here/$name" $extra "$work/$name.work" > "$work/$name.log" 2>&1; then
        status=1
        grep -E '^FAIL|Traceback|Error' "$work/$name.log" || tail -n 20 "$work/$name.log"
    fi
    echo "   checks passed $(grep -c '^OK' "$work/$name.log"), failed $(grep -c '^FAIL' "$work/$name.log")"
done
[ "$status" -eq 0 ] && echo "All passed." || echo "Something failed; logs are in $work."
exit "$status"
