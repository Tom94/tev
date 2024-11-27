{
  description = "Nix tev dev env";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            gcc
            gdb
            cmake
            pkg-config
            binutils
            zlib

            xorg.libX11.dev
            xorg.libXi.dev
            xorg.libXrandr.dev
            xorg.libXinerama.dev
            xorg.libXcursor.dev
            xorg.libXext.dev
            xorg.libXfixes.dev
            xorg.libXrender.dev

            libGL
          ];

          shellHook = ''
            export CMAKE_PREFIX_PATH="${pkgs.xorg.libX11.dev}:${pkgs.xorg.libXi.dev}:${pkgs.libGL}:''${CMAKE_PREFIX_PATH:-}"
          '';
        };
      }
    );
}
