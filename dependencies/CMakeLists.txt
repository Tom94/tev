# tev -- the EXR viewer
#
# Copyright (C) 2025 Thomas Müller <contact@tom94.net>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

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

include(ExternalProject)
get_cmake_property(IS_MULTI GENERATOR_IS_MULTI_CONFIG)

# Compile nanogui
set(NANOGUI_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_SHARED OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_PYTHON OFF CACHE BOOL " " FORCE)
set(NANOGUI_INSTALL OFF CACHE BOOL " " FORCE)
add_subdirectory(nanogui)

set_property(TARGET nanogui glfw glfw_objects PROPERTY FOLDER "dependencies")

# Compile zlib (only on Windows)
if (WIN32)
    set(ZLIB_BUILD_STATIC_LIBS ON CACHE BOOL " " FORCE)
    set(ZLIB_BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL " " FORCE)

    add_subdirectory(zlib)
    set_property(TARGET zlibstatic PROPERTY FOLDER "dependencies")
endif()

# Compile DirectXTex (only on Windows)
if (WIN32)
    set(BUILD_TOOLS OFF CACHE BOOL " " FORCE)
    set(BUILD_DX11 OFF CACHE BOOL " " FORCE)
    set(BUILD_DX12 OFF CACHE BOOL " " FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL " " FORCE)

    add_subdirectory(DirectXTex)
    set_property(TARGET DirectXTex PROPERTY FOLDER "dependencies")
endif()

# Compile aom (dependency of libheif, which follows)
if (TEV_SUPPORT_AVIF)
    set(AOM_TARGET_NAME aom)
    set(AOM_PREFIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/${AOM_TARGET_NAME})
    set(AOM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/aom)
    set(AOM_BINARY_DIR ${AOM_PREFIX_DIR}/src/${AOM_TARGET_NAME}-build)
    set(AOM_INCLUDE_DIR ${AOM_SOURCE_DIR})
    # if(MSVC)
    #     set(AOM_LIB ${CMAKE_STATIC_LIBRARY_PREFIX}aom-static${CMAKE_STATIC_LIBRARY_SUFFIX})
    # else()
        set(AOM_LIB ${CMAKE_STATIC_LIBRARY_PREFIX}aom${CMAKE_STATIC_LIBRARY_SUFFIX})
    # endif()
    if(IS_MULTI)
        set(AOM_LIB_PREFIX ${AOM_BINARY_DIR}/$<CONFIG>/)
    else()
        set(AOM_LIB_PREFIX ${AOM_BINARY_DIR}/)
    endif()
    set(AOM_LIBRARY ${AOM_LIB_PREFIX}${AOM_LIB})

    ExternalProject_Add(${AOM_TARGET_NAME}
        PREFIX ${AOM_PREFIX_DIR}
        SOURCE_DIR ${AOM_SOURCE_DIR}
        BINARY_DIR ${AOM_BINARY_DIR}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target aom
        CMAKE_ARGS
            -DAOM_TARGET_CPU=generic
            -DBUILD_SHARED_LIBS=0
            -DENABLE_DOCS=0
            -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        BUILD_BYPRODUCTS ${AOM_LIBRARY}
        INSTALL_COMMAND ""
    )
endif()

# Compile libde265 (dependency of libheif, which follows)
if (TEV_SUPPORT_HEIC)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(ENABLE_SDL OFF CACHE BOOL " " FORCE)
    add_subdirectory(libde265 EXCLUDE_FROM_ALL)

    set(LIBDE265_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libde265" CACHE PATH " " FORCE)
    set(LIBDE265_LIBRARY de265)
endif()

# Compile JPEG XL library
if (TEV_SUPPORT_JXL)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
    set(BUILD_TESTING OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_FUZZERS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_VIEWERS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_SKCMS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL " " FORCE)

    add_subdirectory(libjxl EXCLUDE_FROM_ALL)
    set_property(TARGET jxl jxl_dec jxl_threads PROPERTY FOLDER "dependencies")
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
    target_include_directories(heif INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/libheif/libheif/api
        ${CMAKE_CURRENT_BINARY_DIR}/libheif
    )

    if (TEV_SUPPORT_AVIF)
        add_dependencies(heif ${AOM_TARGET_NAME})
        target_include_directories(heif PRIVATE ${AOM_BINARY_DIR})
    endif()

    if (TEV_SUPPORT_HEIC)
        target_include_directories(heif PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libde265)
    endif()

    # set(LIBHEIF_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/libheif/libheif/api" "${CMAKE_CURRENT_BINARY_DIR}/libheif" PARENT_SCOPE)

    # Compile Little-CMS for ICC color profile handling. The library name is set to lcms2-tev to avoid conflicts with libjxl which also
    # comes with lcms2.
    file(GLOB LCMS_SRCS
        Little-CMS/src/*.c Little-CMS/include/*.h
        Little-CMS/plugins/fast_float/src/*.c Little-CMS/plugins/fast_float/include/*.h
    )

    add_library(lcms2-tev STATIC ${LCMS_SRCS})

    # We don't use the SSE2 components of CMS in tev; disable to simplify ARM compilation
    target_compile_definitions(lcms2-tev PRIVATE -DCMS_DONT_USE_SSE2=1)
    target_include_directories(lcms2-tev PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/include
        ${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/plugins/fast_float/include
    )
endif()

# Compile libexif for Exif metadata parsing
add_subdirectory(libexif EXCLUDE_FROM_ALL)

# Compile libultrahdr for HDR JPG support
set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
set(UHDR_BUILD_DEPS ON CACHE BOOL " " FORCE)
set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(UHDR_BUILD_TESTS OFF CACHE BOOL " " FORCE)
set(UHDR_ENABLE_INSTALL OFF CACHE BOOL " " FORCE)
add_subdirectory(libultrahdr EXCLUDE_FROM_ALL)

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
set(CLIP_X11_WITH_PNG OFF CACHE BOOL " " FORCE)
add_subdirectory(clip)
target_include_directories(clip INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clip)

