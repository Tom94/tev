{ lib
, stdenv
, cmake
, darwin
, fetchFromGitHub
, lcms2
, libGL
, libX11
, libffi
, libxkbcommon
, perl
, pkg-config
, wayland
, wayland-protocols
, wayland-scanner
, wrapGAppsHook3
, xorg
, zenity
,
}:

stdenv.mkDerivation rec {
  # Content to be used if in nixpkgs
  # version = "2.3";
  # src = fetchFromGitHub {
  #   owner = "Tom94";
  #   repo = "tev";
  #   rev = "v${version}";
  #   fetchSubmodules = true;
  #   hash = "sha256-wEvFeY7b9Au8ltg7X8EaPuifaDKKYOuDyhXK6gGf7Wo=";
  # };

  # Extract version from CMakeLists.txt
  version = with builtins;
    let
      cmakeContents = readFile ./CMakeLists.txt;
      versionMatch = match ".*VERSION[[:space:]]+([0-9]+\\.[0-9]+).*" cmakeContents;
    in
    if versionMatch == null then throw "Could not find version in CMakeLists.txt"
    else head versionMatch;

  src = ./.;

  pname = "tev";

  nativeBuildInputs = [
    cmake
    perl
    pkg-config
    wrapGAppsHook3
  ];

  buildInputs =
    [
      lcms2
    ]
    ++ lib.optionals stdenv.isLinux [
      libX11
      xorg.libXrandr
      xorg.libXinerama
      xorg.libXcursor
      xorg.libXi
      wayland
      wayland-protocols
      wayland-scanner
      libxkbcommon
      libffi
    ]
    ++ lib.optionals stdenv.isDarwin [
      darwin.apple_sdk.frameworks.Cocoa
      darwin.apple_sdk.frameworks.Metal
      darwin.apple_sdk.frameworks.OpenGL
    ];

  dontWrapGApps = true; # We also need zenity (see below)

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DTEV_DEPLOY=1"
  ];

  postInstall = lib.optionalString stdenv.isLinux ''
    wrapProgram $out/bin/tev \
      "''${gappsWrapperArgs[@]}" \
      --prefix PATH ":" "${zenity}/bin" \
      --prefix LD_LIBRARY_PATH ":" "${lib.makeLibraryPath [
        wayland
        libxkbcommon
        libGL
        xorg.libX11
        xorg.libXcursor
        xorg.libXi
        xorg.libXinerama
        xorg.libXrandr
      ]}"
  '';

  env.CXXFLAGS = "-include cstdint";

  meta = {
    description = "High dynamic range (HDR) image viewer for people who care about colors";
    mainProgram = "tev";
    longDescription = ''
      High dynamic range (HDR) image viewer for people who care about colors. It is
      - Lightning fast: starts up instantly, loads hundreds of images in seconds.
      - Accurate: understands color profiles and displays HDR.
      - Versatile: supports many formats, histograms, pixel peeping, tonemaps, etc.
    '';
    changelog = "https://github.com/Tom94/tev/releases/tag/v${version}";
    homepage = "https://github.com/Tom94/tev";
    license = lib.licenses.gpl3;
    maintainers = with lib.maintainers; [ ];
    platforms = lib.platforms.unix;
  };
}
