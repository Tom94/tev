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

# Compile aom (dependency of libheif, which follows)
if (TEV_SUPPORT_AVIF)
    set(AOM_TARGET_CPU "generic" CACHE STRING " " FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(ENABLE_DOCS OFF CACHE BOOL " " FORCE)
    add_subdirectory(aom EXCLUDE_FROM_ALL)

    set(AOM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/aom" CACHE PATH " " FORCE)
    set(AOM_LIBRARY aom)
endif()

# Compile libde265 (dependency of libheif, which follows)
if (TEV_SUPPORT_HEIC)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(ENABLE_SDL OFF CACHE BOOL " " FORCE)
    add_subdirectory(libde265 EXCLUDE_FROM_ALL)

    set(LIBDE265_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libde265" CACHE PATH " " FORCE)
    set(LIBDE265_LIBRARY de265)
endif()

if (TEV_USE_LIBHEIF)
    # Compile libheif

    # General build config
    set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(BUILD_TESTING OFF CACHE BOOL " " FORCE)
    set(WITH_EXAMPLES OFF CACHE BOOL " " FORCE)
    set(WITH_FUZZERS OFF CACHE BOOL " " FORCE)
    set(WITH_GDK_PIXBUF OFF CACHE BOOL " " FORCE)
    set(WITH_INSTALL OFF CACHE BOOL " " FORCE)

    # Codecs
    set(WITH_LIBDE265 ${TEV_SUPPORT_HEIC})
    set(WITH_X265 OFF CACHE BOOL " " FORCE)
    set(WITH_OpenH264_ENCODER OFF CACHE BOOL " " FORCE)
    set(WITH_OpenH264_DECODER OFF CACHE BOOL " " FORCE)

    set(WITH_AOM_ENCODER OFF CACHE BOOL " " FORCE)
    set(WITH_AOM_DECODER ${TEV_SUPPORT_AVIF} CACHE BOOL " " FORCE)

    # TODO: Also consider adding JPEG2000 support via libheif.

    # libsharpyuv is not needed for us, because we're not interested in converting from RGB to YUV. tev is primarily an image viewer that
    # goes from YUV to RGB upon loading images, where libsharpyuv does nothing. See https://www.ctrl.blog/entry/webp-sharp-yuv.html for more
    # info.
    set(WITH_LIBSHARPYUV OFF CACHE BOOL " " FORCE)

    add_subdirectory(libheif EXCLUDE_FROM_ALL)
    target_include_directories(heif PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aom")
    target_include_directories(heif PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/libde265")

    set(LIBHEIF_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/libheif/libheif/api" "${CMAKE_CURRENT_BINARY_DIR}/libheif" PARENT_SCOPE)

    # Compile Little-CMS for ICC color profile handling

    # WARNING: Enabling the fast float plugin (GPLv3) causes a License conflict with the rest of tev (BSD 3-clause). Therefore, we disable
    # it here for now. Currently seeking to purchase a more permissive license as per https://www.littlecms.com/plugin/. Will enable once
    # this is resolved.
    file(GLOB LCMS_SRCS
        Little-CMS/src/*.c Little-CMS/include/*.h
        # Little-CMS/plugins/fast_float/src/*.c Little-CMS/plugins/fast_float/include/*.h
    )

    file(GLOB LCMS_HDRS
        Little-CMS/include/*.h
        # Little-CMS/plugins/fast_float/include/*.h
    )

    add_library(lcms2 STATIC ${LCMS_SRCS} ${LCMS_HDRS})
    set_target_properties(lcms2 PROPERTIES PUBLIC_HEADER "${LCMS_HDRS}")
    target_include_directories(lcms2 PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/include"
        # "${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/plugins/fast_float/include"
    )
endif()

# Compile OpenEXR
set(IMATH_INSTALL OFF CACHE BOOL " " FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL " " FORCE)
add_subdirectory(Imath)

set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL " " FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL " " FORCE)
add_subdirectory(libdeflate EXCLUDE_FROM_ALL)

set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL " " FORCE)
set(EXR_DEFLATE_LIB libdeflate::libdeflate_static)

set(OPENEXR_INSTALL OFF CACHE BOOL " " FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL " " FORCE)
set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL " " FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL " " FORCE)
add_subdirectory(openexr)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # Fix compilation on Linux when code security checks are enabled
    target_compile_definitions(OpenEXRCore PRIVATE _GNU_SOURCE)
endif()

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
