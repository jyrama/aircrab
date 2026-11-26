#!/bin/sh
# Golden CLI harness for shairport-sync. Usage: harness.sh build|capture|check
#   build    configure+make into ./build (override flags via CONFIGURE_FLAGS, binary via BIN)
#   capture  run the matrix and record stdout/stderr/exit code into ./golden
#   check    run the matrix and diff against ./golden; exit 1 on any difference
# Capture the golden files on the reference machine (the Pi), then run check
# after every change. Live cases run under timeout and must exit with 124.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
mode=${1:-check}
CONFIGURE_FLAGS=${CONFIGURE_FLAGS:---with-stdout --with-dummy --with-ssl=openssl --without-ffmpeg --without-systemd-startup --without-systemv-startup}
TIMEOUT=${TIMEOUT:-8}
build="$here/build"
case $mode in
  build|capture|check) ;;
  *) echo "usage: $0 build|capture|check" >&2; exit 2 ;;
esac
if [ -n "${BIN:-}" ]; then
  bin=$BIN
else
  bin="$build/shairport-sync"
  if [ "$mode" != check ] || [ ! -x "$bin" ]; then
    mkdir -p "$build"
    cd "$build"
    if [ ! -f Makefile ]; then "$root/configure" $CONFIGURE_FLAGS >/dev/null; fi
    make -s
    cd "$here"
  fi
fi
norm() {
  sed -E -e 's/^[[:space:]]*[0-9]+\.[0-9]+[[:space:]]+//' -e 's/\[[[:space:]]*[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+\]//g' "$1"
}
run_matrix() {
  outdir=$1
  mkdir -p "$outdir"
  cd "$here"
  while IFS='|' read -r name args; do
    case $name in ''|\#*) continue ;; esac
    eval "set -- $args"
    set +e
    timeout "$TIMEOUT" "$bin" "$@" >"$outdir/$name.stdout.raw" 2>"$outdir/$name.stderr.raw"
    echo $? >"$outdir/$name.exit"
    set -e
    norm "$outdir/$name.stdout.raw" >"$outdir/$name.stdout"
    norm "$outdir/$name.stderr.raw" >"$outdir/$name.stderr"
    rm -f "$outdir/$name.stdout.raw" "$outdir/$name.stderr.raw"
  done < "$here/matrix.txt"
}
case $mode in
  build) exit 0 ;;
  capture)
    rm -rf "$here/golden"
    run_matrix "$here/golden"
    echo "golden captured in $here/golden"
    ;;
  check)
    if [ ! -d "$here/golden" ]; then
      echo "no golden dir -- run: $0 capture (on the reference machine)" >&2
      exit 2
    fi
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    run_matrix "$tmp"
    fails=0
    for f in "$here"/golden/*.stdout "$here"/golden/*.stderr "$here"/golden/*.exit; do
      name=$(basename "$f")
      if ! diff -u "$f" "$tmp/$name" >/dev/null 2>&1; then
        echo "FAIL: $name"
        diff -u "$f" "$tmp/$name" | head -20
        fails=$((fails + 1))
      fi
    done
    if [ "$fails" -gt 0 ]; then
      echo "$fails case(s) differ from golden"
      exit 1
    fi
    echo "all cases match golden"
    ;;
esac
