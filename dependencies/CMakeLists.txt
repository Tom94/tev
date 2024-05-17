# Some components of this file were taken from PBRT-v3 (BSD 2-clause "Simplified" License)
# PBRT-v3 can be found here: https://github.com/mmp/pbrt-v3
cmake_minimum_required(VERSION 3.13)

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nanogui/CMakeLists.txt")
    message(FATAL_ERROR
        "Some tev dependencies are missing. "
        "If you forgot the '--recursive' flag when cloning this project, "
        "this can be fixed by calling 'git submodule update --init --recursive'."
    )
endif()

# Compile nanogui
set(NANOGUI_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_SHARED OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_PYTHON OFF CACHE BOOL " " FORCE)
set(NANOGUI_INSTALL OFF CACHE BOOL " " FORCE)
add_subdirectory(nanogui)

set(NANOGUI_TARGETS nanogui glfw glfw_objects)
set_property(TARGET ${NANOGUI_TARGETS} PROPERTY FOLDER "dependencies")

# Compile zlib (only on Windows)
if (WIN32)
    set(ZLIB_BUILD_STATIC_LIBS ON CACHE BOOL " " FORCE)
    set(ZLIB_BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL " " FORCE)
    add_subdirectory(zlib)
    set_property(TARGET zlibstatic PROPERTY FOLDER "dependencies")

    set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zlib" CACHE PATH " " FORCE)
    set(ZLIB_LIBRARY zlibstatic)

    include_directories(${ZLIB_INCLUDE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/zlib")
endif()

# Compile DirectXTex (only on Windows)
if (WIN32)
    set(BUILD_TOOLS OFF CACHE BOOL " " FORCE)
    set(BUILD_DX11 OFF CACHE BOOL " " FORCE)
    set(BUILD_DX12 OFF CACHE BOOL " " FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL " " FORCE)
    add_subdirectory(DirectXTex)
    set_property(TARGET DirectXTex PROPERTY FOLDER "dependencies")

    set(DIRECTXTEX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DirectXTex/DirectXTex" CACHE PATH " " FORCE)
    set(DIRECTXTEX_LIBRARY DirectXTex)

    include_directories(${DIRECTXTEX_INCLUDE_DIR})
endif()

# Compile OpenEXR
set(IMATH_INSTALL OFF CACHE BOOL " " FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL " " FORCE)
add_subdirectory(Imath)

set(OPENEXR_INSTALL OFF CACHE BOOL " " FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL " " FORCE)
set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL " " FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL " " FORCE)
add_subdirectory(openexr)

# Compile clip
set(CLIP_EXAMPLES OFF CACHE BOOL " " FORCE)
set(CLIP_TESTS OFF CACHE BOOL " " FORCE)
add_subdirectory(clip)

# Manually populate locations of our included dependencies.
set(ARGS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/args PARENT_SCOPE)

set(CLIP_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/clip PARENT_SCOPE)

set(FMT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/fmt/include PARENT_SCOPE)

if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Emscripten")
    set(GLFW_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/nanogui/ext/glfw/include PARENT_SCOPE)
endif()

set(NANOGUI_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/nanogui/ext/nanovg/src
    ${CMAKE_CURRENT_SOURCE_DIR}/nanogui/include
PARENT_SCOPE)
set(NANOGUI_EXTRA_INCS ${NANOGUI_EXTRA_INCS} PARENT_SCOPE)
set(NANOGUI_EXTRA_DEFS ${NANOGUI_EXTRA_DEFS} PARENT_SCOPE)
set(NANOGUI_EXTRA_LIBS ${NANOGUI_EXTRA_LIBS} PARENT_SCOPE)

set(QOI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/qoi PARENT_SCOPE)

set(STB_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/stb PARENT_SCOPE)

set(TINYLOGGER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/tinylogger PARENT_SCOPE)

set(UTFCPP_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/utfcpp/source PARENT_SCOPE)
