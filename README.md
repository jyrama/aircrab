# aircrab

Rust port of [shairport-sync](https://github.com/mikebrady/shairport-sync) (AirPlay 1/2 server)
and [nqptp](https://github.com/mikebrady/nqptp) (ad-hoc PTP server), using the incremental
rsvg-style method: keep a stable C ABI at the seam, port piece by piece behind it, and keep the
test harnesses green at every step.

## Layout

- `abi/` — canonical `nqptp-shm-structures.h`, the single source of truth for the shared-memory
  ABI between nqptp and shairport-sync. `abi/check-sync.sh` fails if the copies inside the legacy
  trees drift from it. Any ABI change bumps `NQPTP_SHM_STRUCTURES_VERSION` and updates all copies.
- `nqptp/` — legacy C tree of nqptp, ported piece by piece. Contains `harness/` (shm/control-port
  test harness, parses `abi/` header for the layout) and the `--ptp-ports=E,G` option so tests
  run without privileged ports.
- `shairport-sync/` — legacy C tree of shairport-sync, ported piece by piece. Contains
  `tests/golden_cli/` (golden CLI test harness for `parse_options()` and startup).
- `rust/` — cargo workspace for the ported Rust code (created when the first crate lands).

Endgame: the separate nqptp process and its shm/UDP-9000 control channel are dropped in favor of
an in-process statime-based PTP client inside the Rust shairport-sync.

Targets: x86-64 and aarch64 GNU/Linux. PipeWire is the only audio backend kept.

## Test harnesses

```
# nqptp: build + run shm/control-port tests; capture golden CLI output once
./nqptp/harness/run.sh capture     # first time only
./nqptp/harness/run.sh check       # after every change

# shairport-sync: capture golden CLI matrix on the reference machine, then check
cd shairport-sync/tests/golden_cli
CONFIGURE_FLAGS='--with-pipewire --with-avahi --with-ssl=openssl' ./harness.sh capture
./harness.sh check

# ABI consistency
./abi/check-sync.sh
```
