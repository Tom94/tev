{
  description = "tev — The EDR Viewer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        package = pkgs.callPackage ./package.nix { };

        ldLibraryPath = pkgs.lib.makeLibraryPath (
          with pkgs;
          [
            wayland
            libxkbcommon
          ]
        );

        commonBuildInputs =
          with pkgs;
          if stdenv.isDarwin then
            [ ]
          else
            [
              binutils
              mesa-demos # for glxinfo, eglinfo
            ];
      in
      {
        packages = {
          default = package;
          tev = package;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ package ];
          buildInputs =
            commonBuildInputs
            ++ (
              with pkgs;
              if stdenv.isDarwin then
                [ ]
              else
                [
                  gcc
                  gdb
                ]
            );
          LD_LIBRARY_PATH = ldLibraryPath;
        };

        devShells.clang =
          pkgs.mkShell.override
            {
              stdenv = pkgs.llvmPackages_22.libcxxStdenv;
            }
            {
              inputsFrom = [ package ];
              buildInputs =
                commonBuildInputs ++ (with pkgs; if stdenv.isDarwin then [ ] else [ llvmPackages_22.lldb ]);
              LD_LIBRARY_PATH = ldLibraryPath;
            };

        apps.default = flake-utils.lib.mkApp {
          drv = package;
        };
      }
    );
}
