# tev -- the EDR viewer
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

# Some dependencies depend on zlib, so we compile it first
set(ZLIB_BUILD_TESTING OFF CACHE BOOL " " FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL " " FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL " " FORCE)
set(ZLIB_BUILD_MINIZIP OFF CACHE BOOL " " FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL " " FORCE)

add_subdirectory(zlib)

# The following variables ensure that find_package can find zlib
set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zlib" CACHE PATH " " FORCE)
set(ZLIB_LIBRARY zlibstatic CACHE STRING " " FORCE)
set(ZLIB_FOUND TRUE CACHE BOOL " " FORCE)
add_library(ZLIB::ZLIB ALIAS zlibstatic)

# Compile nanogui
set(NANOGUI_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_SHARED OFF CACHE BOOL " " FORCE)
set(NANOGUI_BUILD_PYTHON OFF CACHE BOOL " " FORCE)
set(NANOGUI_INSTALL OFF CACHE BOOL " " FORCE)

# tev provides its own stb_image implementation
set(NANOGUI_SKIP_STB_IMAGE_IMPLEMENTATION ON CACHE BOOL " " FORCE)

# Pure nix-darwin environments don't have an easy pathway to install the metal compiler tools. Hence disable precompilation of metal
# shaders. They will be compiled at runtime instead which is sufficiently fast for tev's purposes.
set(NANOGUI_SKIP_METAL_SHADER_PRECOMPILATION ON CACHE BOOL " " FORCE)

add_subdirectory(nanogui)

# Compile DirectXTex (only on Windows)
if (WIN32)
    set(BUILD_TOOLS OFF CACHE BOOL " " FORCE)
    set(BUILD_DX11 OFF CACHE BOOL " " FORCE)
    set(BUILD_DX12 OFF CACHE BOOL " " FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL " " FORCE)

    add_subdirectory(DirectXTex)
endif()

# Compile Little-CMS for ICC color profile handling. The library name is set to lcms2-tev to avoid conflicts with libjxl which also comes
# with lcms2.
file(GLOB LCMS_SRCS
    Little-CMS/src/*.c Little-CMS/include/*.h
    Little-CMS/plugins/fast_float/src/*.c Little-CMS/plugins/fast_float/include/*.h
)

add_library(lcms2-tev STATIC ${LCMS_SRCS})
target_include_directories(lcms2-tev PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/plugins/fast_float/include
)

cmake_host_system_information(RESULT HAS_SSE2 QUERY HAS_SSE2)
if (NOT HAS_SSE2)
    message(STATUS "SSE2 not supported on this system; disabling it in Little-CMS")
    target_compile_definitions(lcms2-tev PRIVATE -DCMS_DONT_USE_SSE2=1)
endif()

# Compile libpng
set(PNG_SHARED OFF CACHE BOOL " " FORCE)
set(PNG_STATIC ON CACHE BOOL " " FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL " " FORCE)
set(PNG_TESTS OFF CACHE BOOL " " FORCE)
set(PNG_TOOLS OFF CACHE BOOL " " FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL " " FORCE)
add_subdirectory(libpng EXCLUDE_FROM_ALL)
add_dependencies(png_static zlibstatic)
target_include_directories(png_static PRIVATE ${ZLIB_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/zlib)

# Compile libexif for Exif metadata parsing
add_subdirectory(libexif EXCLUDE_FROM_ALL)

# Compile libjpeg-turbo for JPEG support
set(JPEGTURBO_TARGET_NAME turbojpeg)
set(JPEGTURBO_PREFIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/${JPEGTURBO_TARGET_NAME})
set(JPEGTURBO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo)
set(JPEGTURBO_BINARY_DIR ${JPEGTURBO_PREFIX_DIR}/src/${JPEGTURBO_TARGET_NAME}-build)
set(JPEG_INCLUDE_DIRS ${JPEGTURBO_SOURCE_DIR}/src ${JPEGTURBO_BINARY_DIR})
if(MSVC)
    set(JPEG_LIB ${CMAKE_STATIC_LIBRARY_PREFIX}jpeg-static${CMAKE_STATIC_LIBRARY_SUFFIX})
else()
    set(JPEG_LIB ${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX})
endif()
if(IS_MULTI)
    set(JPEG_LIB_PREFIX ${JPEGTURBO_BINARY_DIR}/$<CONFIG>/)
else()
    set(JPEG_LIB_PREFIX ${JPEGTURBO_BINARY_DIR}/)
endif()
set(JPEG_LIBRARIES ${JPEG_LIB_PREFIX}${JPEG_LIB})
set(JPEG_VERSION "80")
ExternalProject_Add(${JPEGTURBO_TARGET_NAME}
    PREFIX ${JPEGTURBO_PREFIX_DIR}
    SOURCE_DIR ${JPEGTURBO_SOURCE_DIR}
    BINARY_DIR ${JPEGTURBO_BINARY_DIR}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target jpeg-static
    CMAKE_ARGS
        -DENABLE_SHARED=0
        -DWITH_JPEG8=1
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
    BUILD_BYPRODUCTS ${JPEG_LIBRARIES}
    INSTALL_COMMAND ""
)

# The following variables ensure that find_package can find libjpeg-turbo
set(JPEG_LIBRARY ${JPEG_LIBRARIES})
set(JPEG_INCLUDE_DIR ${JPEGTURBO_SOURCE_DIR})

add_library(jpeg-turbo INTERFACE IMPORTED GLOBAL)
add_dependencies(jpeg-turbo ${JPEGTURBO_TARGET_NAME})
set_target_properties(jpeg-turbo PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${JPEG_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${JPEG_LIBRARIES}"
)
add_library(JPEG::JPEG ALIAS jpeg-turbo)
set(JPEG_FOUND TRUE CACHE BOOL " " FORCE)

# Compile OpenJPEG for JPEG 2000 support
set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
set(BUILD_CODEC OFF CACHE BOOL " " FORCE)
add_subdirectory(openjpeg EXCLUDE_FROM_ALL)

# The following variables ensure that find_package can find OpenJPEG
set(OpenJPEG_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/openjpeg/src/lib/openjp2
    ${CMAKE_CURRENT_BINARY_DIR}/openjpeg/src/lib/openjp2
)

set(OpenJPEG_FOUND TRUE CACHE BOOL " " FORCE)
set(OPENJPEG_FOUND TRUE CACHE BOOL " " FORCE)

add_library(openjpeg-tev INTERFACE IMPORTED GLOBAL)
add_dependencies(openjpeg-tev openjp2)
set_target_properties(openjpeg-tev PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OpenJPEG_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES openjp2
)

# Compile aom (dependency of libheif, which follows)
if (TEV_SUPPORT_AVIF)
    set(AOM_TARGET_NAME aom)
    set(AOM_PREFIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/${AOM_TARGET_NAME})
    set(AOM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/aom)
    set(AOM_BINARY_DIR ${AOM_PREFIX_DIR}/src/${AOM_TARGET_NAME}-build)
    set(AOM_INCLUDE_DIRS ${AOM_SOURCE_DIR})
    set(AOM_LIB ${CMAKE_STATIC_LIBRARY_PREFIX}aom${CMAKE_STATIC_LIBRARY_SUFFIX})
    if(IS_MULTI)
        set(AOM_LIB_PREFIX ${AOM_BINARY_DIR}/$<CONFIG>/)
    else()
        set(AOM_LIB_PREFIX ${AOM_BINARY_DIR}/)
    endif()
    set(AOM_LIBRARIES ${AOM_LIB_PREFIX}${AOM_LIB})

    find_program(NASM_EXECUTABLE nasm)
    if (NASM_EXECUTABLE)
        message(STATUS "NASM found, some decoders will be faster")
    endif()

    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(AOM_TARGET_CPU "arm64")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)$")
        set(AOM_TARGET_CPU "arm")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NASM_EXECUTABLE)
        set(AOM_TARGET_CPU "x86_64")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i386|i486|i586|i686|x86)$" AND NASM_EXECUTABLE)
        set(AOM_TARGET_CPU "x86")
    else()
        set(AOM_TARGET_CPU "generic")
    endif()
    message(STATUS "AOM target CPU: ${AOM_TARGET_CPU}, CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")

    ExternalProject_Add(${AOM_TARGET_NAME}
        PREFIX ${AOM_PREFIX_DIR}
        SOURCE_DIR ${AOM_SOURCE_DIR}
        BINARY_DIR ${AOM_BINARY_DIR}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target aom
        CMAKE_ARGS
            -DAOM_TARGET_CPU=${AOM_TARGET_CPU}
            -DBUILD_SHARED_LIBS=0
            -DENABLE_DOCS=0
            -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
            -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        BUILD_BYPRODUCTS ${AOM_LIBRARIES}
        INSTALL_COMMAND ""
    )

    set(AOM_LIBRARY ${AOM_LIBRARIES} CACHE STRING " " FORCE)
    set(AOM_INCLUDE_DIR ${AOM_INCLUDE_DIRS} CACHE PATH " " FORCE)
    set(AOM_DIR ${AOM_SOURCE_DIR} CACHE PATH " " FORCE)

    add_library(aom-lib INTERFACE IMPORTED GLOBAL)
    add_dependencies(aom-lib ${AOM_TARGET_NAME})
    set_target_properties(aom-lib PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${AOM_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${AOM_LIBRARIES}"
    )

    set(AOM_DECODER_FOUND TRUE CACHE BOOL " " FORCE)
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
    set(JPEGXL_ENABLE_TOOLS ON CACHE BOOL " " FORCE) # needed for gain map support
    set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_SKCMS OFF CACHE BOOL " " FORCE)
    set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL " " FORCE)
    set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL " " FORCE)

    add_subdirectory(libjxl EXCLUDE_FROM_ALL)
endif()

# Compile libheif
if (TEV_USE_LIBHEIF)
    # General build config
    set(BUILD_DOCUMENTATION OFF CACHE BOOL " " FORCE)
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

    set(WITH_JPEG_DECODER ON CACHE BOOL " " FORCE)
    set(WITH_OpenJPEG_DECODER ON CACHE BOOL " " FORCE)

    # libsharpyuv is not needed for us, because we're not interested in converting from RGB to YUV. tev is primarily an image viewer that
    # goes from YUV to RGB upon loading images, where libsharpyuv does nothing. See https://www.ctrl.blog/entry/webp-sharp-yuv.html for more
    # info.
    set(WITH_LIBSHARPYUV OFF CACHE BOOL " " FORCE)

    set(ENABLE_PLUGIN_LOADING OFF CACHE BOOL " " FORCE)
    set(ENABLE_EXPERIMENTAL_MINI_FORMAT ON CACHE BOOL " " FORCE)

    add_subdirectory(libheif EXCLUDE_FROM_ALL)
    target_include_directories(heif INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/libheif/libheif/api
        ${CMAKE_CURRENT_BINARY_DIR}/libheif
    )

    add_dependencies(heif ${JPEGTURBO_TARGET_NAME})
    target_include_directories(heif PRIVATE "${JPEG_INCLUDE_DIRS}")

    add_dependencies(heif openjp2)
    target_include_directories(heif PRIVATE "${OpenJPEG_INCLUDE_DIRS}")

    if (TEV_SUPPORT_AVIF)
        add_dependencies(heif ${AOM_TARGET_NAME})
        target_include_directories(heif PRIVATE ${AOM_BINARY_DIR})
    endif()

    if (TEV_SUPPORT_HEIC)
        target_include_directories(heif PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libde265)
    endif()
endif()

# Compile libwebp
set(WEBP_LINK_STATIC ON CACHE BOOL " " FORCE)
set(WEBP_ENABLE_SIMD ON CACHE BOOL " " FORCE)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_WEBPMUX ON CACHE BOOL " " FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL " " FORCE)
set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL " " FORCE)
set(WEBP_USE_THREAD ON CACHE BOOL " " FORCE)
set(WEBP_NEAR_LOSSLESS ON CACHE BOOL " " FORCE)
set(WEBP_ENABLE_SWAP_16BIT_CSP ON CACHE BOOL " " FORCE)
add_subdirectory(libwebp EXCLUDE_FROM_ALL)

set(WebP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/src)
set(WebP_LIBRARY webpdecoder)
add_library(webp_wrapper INTERFACE)
target_link_libraries(webp_wrapper INTERFACE webpdecoder)
target_include_directories(webp_wrapper INTERFACE ${WebP_INCLUDE_DIR})
add_library(WebP::webp ALIAS webp_wrapper)

# Compile OpenEXR
set(IMATH_INSTALL OFF CACHE BOOL " " FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL " " FORCE)
add_subdirectory(Imath)

set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL " " FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL " " FORCE)
add_subdirectory(libdeflate EXCLUDE_FROM_ALL)

set(Deflate_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libdeflate)
set(Deflate_LIBRARY libdeflate_static)
add_library(deflate_wrapper INTERFACE)
target_link_libraries(deflate_wrapper INTERFACE libdeflate_static)
target_include_directories(deflate_wrapper INTERFACE ${Deflate_INCLUDE_DIR})
add_library(Deflate::Deflate ALIAS deflate_wrapper)

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

# Compile libtiff
set(BUILD_SHARED_LIBS OFF CACHE BOOL " " FORCE)
set(tiff-tools OFF CACHE BOOL " " FORCE)
set(tiff-tests OFF CACHE BOOL " " FORCE)
set(tiff-contrib OFF CACHE BOOL " " FORCE)
set(tiff-docs OFF CACHE BOOL " " FORCE)
set(tiff-deprecated OFF CACHE BOOL " " FORCE)
set(tiff-install OFF CACHE BOOL " " FORCE)
# Disable lzma, webp, and zstd support in libtiff as we don't need them.
set(jbig OFF CACHE BOOL " " FORCE)
set(lerc OFF CACHE BOOL " " FORCE)
set(lzma OFF CACHE BOOL " " FORCE)
set(zstd OFF CACHE BOOL " " FORCE)
set(HAVE_JPEGTURBO_DUAL_MODE_8_12 TRUE CACHE BOOL " " FORCE)
add_subdirectory(libtiff EXCLUDE_FROM_ALL)

# Compile clip
set(CLIP_EXAMPLES OFF CACHE BOOL " " FORCE)
set(CLIP_TESTS OFF CACHE BOOL " " FORCE)
set(CLIP_X11_WITH_PNG ON CACHE BOOL " " FORCE)
set(CLIP_X11_PNG_LIBRARY png_static CACHE STRING " " FORCE)
add_subdirectory(clip)
target_include_directories(clip INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clip)
target_compile_definitions(clip PRIVATE -DHAVE_PNG_H)

# Compile libraw
set(LIBRAW_PATH "${CMAKE_CURRENT_SOURCE_DIR}/LibRaw" CACHE PATH " " FORCE)
set(ENABLE_OPENMP OFF CACHE BOOL " " FORCE)
set(LCMS2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Little-CMS/include" CACHE PATH " " FORCE)
set(LCMS2_LIBRARIES lcms2-tev CACHE STRING " " FORCE)
add_subdirectory(LibRaw-cmake)

# Compile expat (xml parser) and then xmp which depends on it
set(EXPAT_BUILD_DOCS OFF CACHE BOOL " " FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(EXPAT_BUILD_PKGCONFIG OFF CACHE BOOL " " FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL " " FORCE)
set(EXPAT_BUILD_TOOLS OFF CACHE BOOL " " FORCE)
set(EXPAT_ENABLE_INSTALL OFF CACHE BOOL " " FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL " " FORCE)
add_subdirectory(libexpat/expat EXCLUDE_FROM_ALL)
add_subdirectory(xmp EXCLUDE_FROM_ALL)
