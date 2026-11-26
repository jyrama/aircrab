#!/usr/bin/env bash
set -e
cd nqptp
autoreconf -i
cd ..
mkdir -p build-ci/nqptp && cd build-ci/nqptp
../../nqptp/configure --without-systemd-startup
make -j2
./nqptp -V
cd ../..
cd shairport-sync
autoreconf -i
cd ..
mkdir -p build-ci/shairport-sync && cd build-ci/shairport-sync
../../shairport-sync/configure --with-stdout --with-dummy --with-ssl=openssl --with-external-mdns --with-ffmpeg --without-systemd-startup --without-systemv-startup
make -j2
./shairport-sync -V
./shairport-sync -h | head -3
if ./shairport-sync --bogus-option 2>/dev/null; then
  echo "bogus option was accepted" >&2
  exit 1
fi
