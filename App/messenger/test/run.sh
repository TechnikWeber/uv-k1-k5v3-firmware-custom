#!/usr/bin/env bash
# Builds and runs the messenger host tests. Usage: App/messenger/test/run.sh [OUTDIR]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/.."
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"
CC="${CC:-gcc}"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -g -O1 -fstack-protector-all)

build() {   # name sources...
    local name=$1; shift
    "$CC" "${CFLAGS[@]}" -o "$OUT/$name" "$@" -lm
}

build test_proto     "$HERE/test_proto.c"     "$SRC/aprs_ax25.c" "$SRC/aprs_msg.c"
build test_demod     "$HERE/test_demod.c"     "$SRC/aprs_demod.c" "$SRC/aprs_afsk.c" "$SRC/aprs_ax25.c"
build test_mailbox   "$HERE/test_mailbox.c"   "$SRC/mailbox.c"
build test_messenger "$HERE/test_messenger.c" "$SRC/messenger.c" "$SRC/mailbox.c" \
                     "$SRC/aprs_ax25.c" "$SRC/aprs_msg.c"
build test_t9        "$HERE/test_t9.c"        "$SRC/t9.c"

if [[ ! -f "$OUT/audio/cases.txt" ]]; then
    python3 "$HERE/gen_audio.py" "$OUT/audio" > /dev/null
fi

fail=0
"$OUT/test_proto"              || fail=1
"$OUT/test_demod" "$OUT/audio" | grep -v '^  ok' || true
"$OUT/test_demod" "$OUT/audio" > /dev/null || fail=1
"$OUT/test_mailbox"            || fail=1
"$OUT/test_messenger"          || fail=1
"$OUT/test_t9"                 || fail=1

if [[ $fail -eq 0 ]]; then echo "all messenger tests passed"; else echo "TESTS FAILED"; fi
exit $fail
