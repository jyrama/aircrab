#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
out="$here/build"
mkdir -p "$out"
printf '#define CONFIG_FOR_LINUX 1\n#define VERSION "harness"\n#define PACKAGE_NAME "nqptp"\n#define PACKAGE_VERSION "harness"\n' > "$out/config.h"
cd "$root"
${CC:-cc} -fno-common -Wall -Wextra -pthread -I. -I"$out" --include "$out/config.h" \
  -o "$out/nqptp" nqptp.c nqptp-clock-sources.c nqptp-message-handlers.c \
  nqptp-utilities.c general-utilities.c debug.c
