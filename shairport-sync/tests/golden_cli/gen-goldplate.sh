#!/bin/sh
# Generates goldplate cases (goldplate.d/<case>.goldplate) from matrix.txt.
# goldplate specs are JSON: command + arguments + asserts; expected stdout/stderr
# live in separate files (goldplate.d/<case>.stdout/.stderr) that 'goldplate --fix'
# creates on the reference machine.
# Usage:
#   gen-goldplate.sh                                  # (re)generate goldplate.d
#   cp /path/to/built/shairport-sync goldplate.d/shairport-sync-bin
#   goldplate --fix goldplate.d                       # capture expectations (reference machine)
#   goldplate -j4 goldplate.d                         # check (what CI runs once expectations are committed)
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/goldplate.d"
timeout=${TIMEOUT:-8}
rm -rf "$out"
mkdir -p "$out"
while IFS='|' read -r name args; do
  case $name in ''|\#*) continue ;; esac
  # Expected exit codes, from shairport.c: -h/-V exit(EXIT_SUCCESS) when they are
  # argv[1]; -X alone (argc==2) prints config and exit(EXIT_SUCCESS); bad options and
  # config errors go through die() -> EXIT_FAILURE; cases that keep running are
  # killed by timeout -> 124.
  case $name in
    live_*|displayconfig_opts) ec=124 ;;
    bad_*|missing_arg|config_broken) ec=1 ;;
    *) ec=0 ;;
  esac
  eval "set -- $args"
  python3 - "$out/$name.goldplate" "$name" "$ec" "$timeout" "$@" <<'PY'
import json, sys
path, name, ec, timeout = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
args = sys.argv[5:]
# Normalize the same nondeterministic bits harness.sh strips: the leading
# elapsed-time prefix and [HH:MM:SS.mmm] log timestamps (PCRE, multiline).
post = [
    {"pattern": r"^[ \t]*[0-9]+\.[0-9]+[ \t]+", "replacement": ""},
    {"pattern": r"\[[ \t]*[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+\]", "replacement": ""},
]
spec = {
    "working_directory": "..",
    "command": "timeout",
    "arguments": [timeout, "./goldplate.d/shairport-sync-bin"] + args,
    "asserts": [
        {"exit_code": ec},
        {"stdout": "goldplate.d/%s.stdout" % name, "post_process": post},
        {"stderr": "goldplate.d/%s.stderr" % name, "post_process": post},
    ],
}
with open(path, "w") as f:
    json.dump(spec, f, indent=4)
    f.write("\n")
PY
done < "$here/matrix.txt"
echo "wrote $out/*.goldplate -- put the built binary at $out/shairport-sync-bin, then on the"
echo "reference machine run: goldplate --fix $out   (captures *.stdout/*.stderr), commit those;"
echo "check with:          goldplate -j4 $out"
