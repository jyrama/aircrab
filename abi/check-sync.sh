#!/bin/sh
# Verifies the shm ABI header copies in the legacy trees match the canonical
# abi/nqptp-shm-structures.h. Run after any ABI change; bump
# NQPTP_SHM_STRUCTURES_VERSION and update all copies together.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
rc=0
for f in "$root/nqptp/nqptp-shm-structures.h" "$root/shairport-sync/nqptp-shm-structures.h"; do
  if cmp -s "$here/nqptp-shm-structures.h" "$f"; then
    echo "ok    $f matches abi/"
  else
    echo "FAIL  $f differs from abi/nqptp-shm-structures.h"
    rc=1
  fi
done
exit $rc
