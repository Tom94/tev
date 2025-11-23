{ lib
, stdenv
, cmake
, dbus
, fetchFromGitHub
, lcms2
, libGL
, libffi
, libxkbcommon
, nasm
, ninja
, perl
, pkg-config
, wayland
, wayland-protocols
, wayland-scanner
, xorg
,
}:

stdenv.mkDerivation rec {
  pname = "tev";
  # version = "2.6.1";

  # src = fetchFromGitHub {
  #   owner = "Tom94";
  #   repo = "tev";
  #   tag = "v${version}";
  #   fetchSubmodules = true;
  #   hash = "sha256-RRGE/gEWaSwvbytmtR5fLAke8QqIeuYJQzwC++Z1bgc=";
  # };

  # Extract version from CMakeLists.txt
  version = with builtins;
    let
      cmakeContents = readFile ./CMakeLists.txt;
      versionMatch = match ".*VERSION[[:space:]]+([0-9]+\\.[0-9]+\\.[0-9]+).*" cmakeContents;
    in
    if versionMatch == null then throw "Could not find version in CMakeLists.txt"
    else head versionMatch;

  src = ./.;

  postPatch = lib.optionalString stdenv.hostPlatform.isLinux (
    let
      waylandLibPath = "${lib.getLib wayland}/lib";
    in
    ''
      substituteInPlace ./dependencies/nanogui/ext/glfw/src/wl_init.c \
        --replace-fail "libwayland-client.so" "${waylandLibPath}/libwayland-client.so" \
        --replace-fail "libwayland-cursor.so" "${waylandLibPath}/libwayland-cursor.so" \
        --replace-fail "libwayland-egl.so" "${waylandLibPath}/libwayland-egl.so" \
        --replace-fail "libxkbcommon.so" "${lib.getLib libxkbcommon}/lib/libxkbcommon.so"
    ''
  );

  nativeBuildInputs = [
    cmake
    nasm
    ninja
    perl
    pkg-config
  ];

  buildInputs = [
    lcms2
  ] ++ lib.optionals stdenv.hostPlatform.isLinux (
    [
      dbus
      libffi
      libGL
      libxkbcommon
      wayland
      wayland-protocols
      wayland-scanner
    ] ++ (with xorg; [
      libX11
      libXcursor
      libXi
      libXinerama
      libXrandr
    ])
  );

  cmakeFlags = [
    "-DTEV_DEPLOY=1"
  ];

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
    maintainers = with lib.maintainers; [ tom94 ];
    platforms = lib.platforms.unix;
  };
}
