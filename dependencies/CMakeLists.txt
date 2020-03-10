# Some components of this file were taken from PBRT-v3 (BSD 2-clause "Simplified" License)
# PBRT-v3 can be found here: https://github.com/mmp/pbrt-v3
cmake_minimum_required(VERSION 2.8.3)

if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/nanogui")
    message(FATAL_ERROR
        "Some tev dependencies are missing. "
        "If you forgot the '--recursive' flag when cloning this project, "
        "this can be fixed by calling 'git submodule update --init --recursive'."
    )
endif()

# Compile nanogui
set(NANOGUI_BUILD_EXAMPLE OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_SHARED OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_PYTHON OFF CACHE BOOL " " FORCE)
set(NANOGUI_INSTALL OFF CACHE BOOL " " FORCE)
add_subdirectory(nanogui)

set(NANOGUI_TARGETS nanogui nanogui-obj)
if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Emscripten")
    list(APPEND NANOGUI_TARGETS glfw glfw_objects)
endif()
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

# Compile OpenEXR
set(ILMBASE_BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
set(OPENEXR_BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
set(ILMBASE_NAMESPACE_VERSIONING OFF CACHE BOOL " " FORCE)
set(OPENEXR_NAMESPACE_VERSIONING OFF CACHE BOOL " " FORCE)
add_subdirectory(openexr EXCLUDE_FROM_ALL)
set_property(TARGET b44ExpLogTable CopyIlmBaseLibs dwaLookups eLut Half Iex IlmImf IlmImf-obj IlmThread Imath toFloat PROPERTY FOLDER "dependencies")

# Compile clip
set(CLIP_EXAMPLES OFF CACHE BOOL " " FORCE)
set(CLIP_TESTS OFF CACHE BOOL " " FORCE)
add_subdirectory(clip)

# Manually populate locations of our included dependencies.
set(ARGS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/args PARENT_SCOPE)

set(CLIP_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/clip PARENT_SCOPE)

set(EIGEN_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/nanogui/ext/eigen PARENT_SCOPE)

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

set(OPENEXR_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/openexr/IlmBase/Imath
    ${CMAKE_CURRENT_SOURCE_DIR}/openexr/IlmBase/Iex
    ${CMAKE_CURRENT_SOURCE_DIR}/openexr/IlmBase/Half
    ${CMAKE_CURRENT_SOURCE_DIR}/openexr/OpenEXR/IlmImf
    ${CMAKE_CURRENT_BINARY_DIR}/openexr/OpenEXR/config
    ${CMAKE_CURRENT_BINARY_DIR}/openexr/IlmBase/config
PARENT_SCOPE)

set(FILESYSTEM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/filesystem PARENT_SCOPE)

set(STB_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/stb PARENT_SCOPE)

set(TINYFORMAT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/tinyformat PARENT_SCOPE)

set(TINYLOGGER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/tinylogger PARENT_SCOPE)

set(UTFCPP_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/utfcpp/source PARENT_SCOPE)
