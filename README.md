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

## Building

Everything builds with the deps from the root `shell.nix`. With Nix/Lix installed, enter the
dev shell from the repo root:

```
nix-shell          # provides autoconf/automake/libtool/pkg-config/popt/libconfig/openssl/gcc/make + goldplate
```

On a non-Nix system, install the equivalents instead, e.g. Debian/Ubuntu:
`apt install build-essential autoconf automake libtool pkg-config libpopt-dev libconfig-dev libssl-dev`.

One-shot build of both trees + CLI smoke test (same as CI, output in gitignored `build-ci/`):

```
nix-shell --run ./ci/build-and-smoke.sh
# binaries: build-ci/nqptp/nqptp  and  build-ci/shairport-sync/shairport-sync
```

Or build each tree manually (out-of-tree, same as above):

```
# nqptp
cd nqptp && autoreconf -i && cd ..
mkdir -p build-ci/nqptp && cd build-ci/nqptp
../../nqptp/configure --without-systemd-startup && make

# shairport-sync -- minimal set that builds without avahi/pipewire (CI, harness; ffmpeg decoder):
cd shairport-sync && autoreconf -i && cd ..
mkdir -p build-ci/shairport-sync && cd build-ci/shairport-sync
../../shairport-sync/configure --with-stdout --with-dummy --with-ssl=openssl --with-external-mdns --with-ffmpeg --without-systemd-startup --without-systemv-startup && make

# shairport-sync -- real build for the Pi (PipeWire backend, avahi mdns, openssl):
../../shairport-sync/configure --with-pipewire --with-avahi --with-ssl=openssl && make
```

The golden-CLI harness builds shairport-sync into `shairport-sync/tests/golden_cli/build/`
itself: `CONFIGURE_FLAGS='...' shairport-sync/tests/golden_cli/harness.sh build` (see below).
nqptp can also be built without autotools for the harness: `nqptp/harness/build.sh`
(direct cc, output in `nqptp/harness/build/`).

## Test harnesses

```
# nqptp: build + run shm/control-port tests; capture golden CLI output once
./nqptp/harness/run.sh capture     # first time only
./nqptp/harness/run.sh check       # after every change

# shairport-sync: capture golden CLI matrix on the reference machine, then check.
# Use the DEFAULT minimal CONFIGURE_FLAGS (see harness.sh) so the CLI output is
# identical to the CI-built binary (version string and backend list are build flags).
cd shairport-sync/tests/golden_cli
./harness.sh build                                # builds with the minimal default flags
./harness.sh capture
./harness.sh check

# shairport-sync: goldplate golden tests (same matrix, TAP output, for CI)
cd shairport-sync/tests/golden_cli
./gen-goldplate.sh                                # writes goldplate.d/<case>.goldplate (committed)
cp build/shairport-sync goldplate.d/shairport-sync-bin   # the minimal-flags build, as above
nix-shell --run 'goldplate --fix goldplate.d'    # capture expectations ONCE on the reference machine
# the captured goldplate.d/*.stdout and *.stderr land next to the cases; commit them
# with a plain 'git add' (only the test binary is gitignored), then:
nix-shell --run 'goldplate -j4 goldplate.d'       # check (CI runs this automatically)
# live cases use high RTSP ports, a high UDP base, a distinct mDNS name and a /tmp pid dir
# (configs/live.conf), so they can run while the real daemon is up; they are expected to
# hit the timeout (exit 124).

# ABI consistency (byte-identical header copies + compile-time layout lock, SMI v11)
./abi/check-sync.sh
cc -I abi -fsyntax-only abi/check-layout.c
```

## CI

`.github/workflows/ci.yml` (see PR [#1](https://github.com/jyrama/aircrab/pull/1)) runs on every
push to `main`/`vibe/**` and every PR, on `ubuntu-24.04` and `ubuntu-24.04-arm`:

- **harness** — `abi/check-sync.sh` + `nqptp/harness/run.sh check` (no Nix needed).
- **shairport-build** — installs [Lix](https://lix.systems) with
  `samueldr/lix-gha-installer-action@v2026-06-15`, the action listed in the Lix wiki, which runs
  the official `lix-project/lix-installer` in its default multi-user mode. `extra_nix_config` adds
  `cache.lix.systems` as a substituter so packages are fetched prebuilt instead of compiled.
  `shell.nix` pins nixpkgs to a fixed rev via `fetchTarball` (no channels, no `<nixpkgs>`), and
  `nix-community/cache-nix-action@v7` caches `/nix` under `lix-<os>-<arch>-<hash of shell.nix>`.
  The job builds both trees with `nix-shell --run ./ci/build-and-smoke.sh` and smoke-tests the
  CLI; it also builds a PipeWire-only shairport-sync variant (`build-ci/shairport-sync-pw`,
  `--with-pipewire --with-avahi --with-ssl=openssl --with-ffmpeg`) since PipeWire is the only
  backend kept. Once goldplate expectations are committed it also runs the goldplate suite.
