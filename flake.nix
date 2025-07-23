{
  description = "tev — The EDR Viewer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        package = pkgs.callPackage ./package.nix { };
      in
      {
        packages = {
          default = package;
          tev = package;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ package ];
          buildInputs = with pkgs; if stdenv.isDarwin then [ ] else [
            gcc
            gdb
            binutils
            mesa-demos # for glxinfo, eglinfo
          ];
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [
            wayland
            libxkbcommon
          ]);
        };

        apps.default = flake-utils.lib.mkApp {
          drv = package;
        };
      }
    );
}
