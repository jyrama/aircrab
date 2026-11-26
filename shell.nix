# aircrab dev shell: build deps for the C trees + goldplate test runner.
# nixpkgs pinned to nixos-25.05 commit ac62194c (same pin as the old flake.lock).
{ pkgs ? import (fetchTarball "https://github.com/NixOS/nixpkgs/archive/ac62194c3917d5f474c1a844b6fd6da2db95077d.tar.gz") { } }:

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
    pipewire
    avahi
    gcc
    gnumake
    haskellPackages.goldplate
  ];
}
