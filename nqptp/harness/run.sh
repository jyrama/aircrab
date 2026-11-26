#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-check}
"$here/build.sh"
exec python3 "$here/shm_harness.py" --mode "$mode" --binary "$here/build/nqptp" \
  --header "$here/../nqptp-shm-structures.h" --golden "$here/golden"
