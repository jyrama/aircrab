#!/bin/sh
# Generates goldplate cases (goldplate.d/<case>.goldplate) from matrix.txt.
# Usage: gen-goldplate.sh   (then place the built binary at goldplate.d/shairport-sync-bin)
# Capture expectations on the reference machine: goldplate --fix goldplate.d
# Check afterwards:                                 goldplate goldplate.d
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/goldplate.d"
rm -rf "$out"
mkdir -p "$out"
while IFS='|' read -r name args; do
  case $name in ''|\#*) continue ;; esac
  {
    printf 'working_directory: ..\n'
    printf 'command: timeout %s ./goldplate.d/shairport-sync-bin %s\n' "${TIMEOUT:-8}" "$args"
  } > "$out/$name.goldplate"
done < "$here/matrix.txt"
echo "wrote $out/*.goldplate -- put the binary at $out/shairport-sync-bin, then run:"
echo "  goldplate --fix $out   # capture expected output (reference machine)"
echo "  goldplate $out         # check"
