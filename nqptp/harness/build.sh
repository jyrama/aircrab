#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
out="$here/build"
mkdir -p "$out"
if command -v autoreconf >/dev/null 2>&1; then
  mkdir -p "$out/auto"
  cd "$out/auto"
  if [ ! -f Makefile ]; then
    "$root/configure" --without-systemd-startup >/dev/null
  fi
  make -s
  cp nqptp "$out/nqptp"
else
  printf '#define CONFIG_FOR_LINUX 1\n#define VERSION "harness"\n#define PACKAGE_NAME "nqptp"\n#define PACKAGE_VERSION "harness"\n' > "$out/config.h"
  cd "$root"
  ${CC:-cc} -fno-common -Wall -Wextra -pthread -I. -I"$out" --include "$out/config.h" \
    -o "$out/nqptp" nqptp.c nqptp-clock-sources.c nqptp-message-handlers.c \
    nqptp-utilities.c general-utilities.c debug.c
fi
