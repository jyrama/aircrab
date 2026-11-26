{
  description = "aircrab — Rust port of shairport-sync + nqptp (AirPlay 1/2 server, PTP timing)";

  # pinned by rev (lon-style) — nixos-25.05 @ ac62194c
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/ac62194c3917d5f474c1a844b6fd6da2db95077d";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            autoconf
            automake
            libtool
            m4
            pkg-config
            popt
            libconfig
            openssl
            gcc
            gnumake
            haskellPackages.goldplate
          ];
        };
      });
    };
}
