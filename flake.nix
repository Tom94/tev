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

        # Extract version from CMakeLists.txt
        version = with builtins;
          let
            cmakeContents = readFile ./CMakeLists.txt;
            versionMatch = match ".*VERSION[[:space:]]+([^[:space:]]+).*" cmakeContents;
          in
          if versionMatch == null then throw "Could not find version in CMakeLists.txt"
          else head versionMatch;

        # Common dependencies shared between dev shell and build
        commonDeps = with pkgs;
          if stdenv.isDarwin then [
            darwin.apple_sdk.frameworks.Cocoa
            darwin.apple_sdk.frameworks.OpenGL
          ] else [
            pkg-config
            libGL
            perl
            # X11 libraries
            xorg.libX11
            xorg.libXcursor
            xorg.libXi
            xorg.libXinerama
            xorg.libXrandr
            # Wayland libraries
            libxkbcommon
            libffi
            wayland
            wayland-protocols
            wayland-scanner
          ];

        # Common build inputs shared between dev shell and build
        commonBuildInputs = with pkgs; [
          cmake
        ];

        tev = pkgs.stdenv.mkDerivation {
          pname = "tev";
          inherit version;

          src = ./.;

          nativeBuildInputs = commonBuildInputs;
          buildInputs = commonDeps;

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DTEV_DEPLOY=ON"
          ];

          meta = with pkgs.lib; {
            description = "High dynamic range (HDR) image comparison tool for graphics people";
            homepage = "https://github.com/Tom94/tev";
            license = licenses.gpl3;
            platforms = platforms.unix;
            maintainers = [ ];
          };
        };
      in
      {
        packages = {
          default = tev;
          tev = tev;
        };

        devShell = pkgs.mkShell {
          buildInputs = commonDeps ++ commonBuildInputs ++ (with pkgs;
            if stdenv.isDarwin then [ ] else [
              gcc
              gdb
              binutils
              mesa-demos # for glxinfo, eglinfo
            ]
          );
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [
            wayland
            libxkbcommon
          ]);
        };

        apps.default = flake-utils.lib.mkApp {
          drv = tev;
        };
      }
    );
}
