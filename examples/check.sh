#!/bin/sh
# Regenerates every example recording from its script and compares the decoder's
# output against the checked-in expected files.
#
#   examples/check.sh [path-to-blackspan-binary]
#
# Run from the repository root. Exits non-zero on the first difference, so it
# doubles as the CI gate that keeps the wire format, the decoder, and the
# documented output in step. Pass --update to refresh the expected files after
# an intentional change.

set -eu

BIN=build/blackspan
UPDATE=0
for arg in "$@"; do
  case "$arg" in
    --update) UPDATE=1 ;;
    *) BIN=$arg ;;
  esac
done

if [ ! -x "$BIN" ]; then
  echo "examples/check.sh: no blackspan binary at $BIN" >&2
  echo "build it first: cmake -S . -B build && cmake --build build" >&2
  exit 2
fi

EXAMPLES="nominal fleet degraded"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

status=0

for name in $EXAMPLES; do
  script="examples/$name.bsr"
  recording="examples/$name.bsp"

  # 1. The script must still assemble to exactly the committed recording.
  "$BIN" assemble "$script" --out "$WORK/$name.bsp" >/dev/null
  if [ "$UPDATE" = "1" ]; then
    cp "$WORK/$name.bsp" "$recording"
  elif ! cmp -s "$WORK/$name.bsp" "$recording"; then
    echo "FAIL: $script no longer assembles to $recording" >&2
    status=1
    continue
  fi

  # 2. The decoder's output must still match the expected transcript.
  out="$WORK/$name.cli.txt"
  {
    echo "\$ blackspan frames $name.bsp"
    "$BIN" frames "$recording"
    echo
    echo "\$ blackspan sessions $name.bsp"
    "$BIN" sessions "$recording"
    echo
    echo "\$ blackspan schemas $name.bsp"
    "$BIN" schemas "$recording"
    echo
    echo "\$ blackspan channels $name.bsp"
    "$BIN" channels "$recording"
    echo
    echo "\$ blackspan faults $name.bsp"
    "$BIN" faults "$recording"
    echo
    echo "\$ blackspan report $name.bsp --format text"
    "$BIN" report "$recording" --format text
    echo
    echo "\$ blackspan report $name.bsp --format json"
    "$BIN" report "$recording" --format json
    echo
    echo "\$ blackspan report $name.bsp --format dot"
    "$BIN" report "$recording" --format dot
    echo
    echo "\$ blackspan timeline $name.bsp"
    "$BIN" timeline "$recording"
    echo
    echo "\$ blackspan query $name.bsp \"select faults\""
    "$BIN" query "$recording" "select faults"
    echo
    echo "\$ blackspan export $name.bsp --channel 1 --format csv"
    "$BIN" export "$recording" --channel 1 --format csv
  } >"$out" 2>&1

  expected="examples/expected/$name.cli.txt"
  if [ "$UPDATE" = "1" ]; then
    cp "$out" "$expected"
    echo "updated $expected"
  elif ! diff -u "$expected" "$out"; then
    echo "FAIL: $name output differs from $expected" >&2
    status=1
  fi
done

if [ "$status" = "0" ] && [ "$UPDATE" = "0" ]; then
  echo "examples: all $(echo $EXAMPLES | wc -w | tr -d ' ') recordings match"
fi
exit $status
