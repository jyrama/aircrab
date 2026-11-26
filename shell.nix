# aircrab dev shell: build deps for the C trees + goldplate test runner.
# nixpkgs is pinned by lon: lon.nix/lon.lock (SRI hash), updated by the
# lon bot workflow (.github/workflows/lon-update.yml). For local development,
# override with LON_OVERRIDE_nixpkgs=/path/to/nixpkgs.
let
  sources = import ./lon.nix;
in
{ pkgs ? import sources.nixpkgs { } }:

pkgs.mkShell {
  packages = with pkgs; [
    autoconf
    automake
    libtool
    m4
    pkg-config
    popt
    libconfig
    openssl
    ffmpeg
    gcc
    gnumake
    haskellPackages.goldplate
  ];
}
