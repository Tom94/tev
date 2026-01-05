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

project(
    tev
    VERSION 2.7.0
    DESCRIPTION "High dynamic range (HDR) image viewer for people who care about colors."
    LANGUAGES C CXX
)
set(TEV_VERSION "${CMAKE_PROJECT_VERSION}")

if (APPLE)
    enable_language(OBJCXX OBJC)
endif()

if (TEV_DEPLOY)
    set(TEV_VERSION_DEV "${TEV_VERSION}")
else()
    set(TEV_VERSION_DEV "${TEV_VERSION}dev")
endif()

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TEV_VERSION_ARCH "${TEV_VERSION_DEV} (64 bit)")
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(TEV_VERSION_ARCH "${TEV_VERSION_DEV} (32 bit)")
endif()

option(TEV_LINUX_REVERSE_DNS_APP_ID OFF)
set(TEV_LINUX_APP_ID "tev")
if (TEV_LINUX_REVERSE_DNS_APP_ID)
    set(TEV_LINUX_APP_ID "io.github.tom94.tev")
endif()

option(TEV_SUPPORT_AVIF "Support loading AVIF images" ON)
option(TEV_SUPPORT_HEIC "Support loading HEIC images" OFF)
option(TEV_SUPPORT_JXL "Support loading/saving JPEG XL images" ON)

option(TEV_CREATE_METAINFO "Auto-generate metainfo for flatpak release" OFF)
option(TEV_VALIDATE_METAINFO "Validate generated metainfo for flatpak release" OFF)

set(TEV_USE_LIBHEIF OFF)
if (TEV_SUPPORT_AVIF OR TEV_SUPPORT_HEIC)
    set(TEV_USE_LIBHEIF ON)
endif()

# Set ourselves as the startup project in visual studio.
# Not available until cmake 3.6, but doesn't break older versions.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT tev)

if (CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

# Set a default configuration if none was specified
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No release type specified. Setting to 'Release'.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
endif()

if (APPLE)
    set(CMAKE_MACOSX_RPATH ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    # Disable annoying secure CRT warnings
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_CRT_SECURE_NO_WARNINGS")

    # Parallel build
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP24")

    # Static build
    set(CompilerFlags
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
    )

    foreach (CompilerFlag ${CompilerFlags})
        string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
    endforeach()

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Prefer libc++ in conjunction with Clang
include(CheckCXXSourceRuns)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

macro(CHECK_CXX_COMPILER_AND_LINKER_FLAGS _RESULT _CXX_FLAGS _LINKER_FLAGS)
    set(CMAKE_REQUIRED_FLAGS ${_CXX_FLAGS})
    set(CMAKE_REQUIRED_LIBRARIES ${_LINKER_FLAGS})
    set(CMAKE_REQUIRED_QUIET TRUE)
    check_cxx_source_runs("#include <iostream>\nint main(int argc, char **argv) { std::cout << \"test\"; return 0; }" ${_RESULT})
    set(CMAKE_REQUIRED_FLAGS "")
    set(CMAKE_REQUIRED_LIBRARIES "")
endmacro()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+")
    CHECK_CXX_COMPILER_AND_LINKER_FLAGS(HAS_LIBCPP "-stdlib=libc++" "-stdlib=libc++")
    if (APPLE OR HAS_LIBCPP)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -D_LIBCPP_VERSION")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++")
        message(STATUS "TEV: using libc++.")
    else()
        CHECK_CXX_COMPILER_AND_LINKER_FLAGS(HAS_LIBCPP_AND_CPPABI "-stdlib=libc++" "-stdlib=libc++ -lc++abi")
        if (HAS_LIBCPP_AND_CPPABI)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -D_LIBCPP_VERSION")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++abi")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++ -lc++abi")
            message(STATUS "TEV: using libc++ and libc++abi.")
        else()
            message(FATAL_ERROR
                "When Clang is used to compile TEV, libc++ "
                "must be available -- GCC's libstdc++ is not supported! (please install "
                "the libc++ development headers, provided e.g. by the packages "
                "'libc++-dev' and 'libc++abi-dev' on Debian/Ubuntu)."
            )
        endif()
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/dependencies)

set(TEV_SOURCES
    include/tev/imageio/AppleMakerNote.h src/imageio/AppleMakerNote.cpp
    include/tev/imageio/ClipboardImageLoader.h src/imageio/ClipboardImageLoader.cpp
    include/tev/imageio/Colors.h src/imageio/Colors.cpp
    include/tev/imageio/EmptyImageLoader.h src/imageio/EmptyImageLoader.cpp
    include/tev/imageio/Exif.h src/imageio/Exif.cpp
    include/tev/imageio/ExrImageLoader.h src/imageio/ExrImageLoader.cpp
    include/tev/imageio/ExrImageSaver.h src/imageio/ExrImageSaver.cpp
    include/tev/imageio/GainMap.h src/imageio/GainMap.cpp
    include/tev/imageio/ImageLoader.h src/imageio/ImageLoader.cpp
    include/tev/imageio/ImageSaver.h src/imageio/ImageSaver.cpp
    include/tev/imageio/JpegTurboImageLoader.h src/imageio/JpegTurboImageLoader.cpp
    include/tev/imageio/PfmImageLoader.h src/imageio/PfmImageLoader.cpp
    include/tev/imageio/PngImageLoader.h src/imageio/PngImageLoader.cpp
    include/tev/imageio/QoiImageLoader.h src/imageio/QoiImageLoader.cpp
    include/tev/imageio/QoiImageSaver.h src/imageio/QoiImageSaver.cpp
    include/tev/imageio/RawImageLoader.h src/imageio/RawImageLoader.cpp
    include/tev/imageio/StbiHdrImageSaver.h src/imageio/StbiHdrImageSaver.cpp
    include/tev/imageio/StbiImageLoader.h src/imageio/StbiImageLoader.cpp
    include/tev/imageio/StbiLdrImageSaver.h src/imageio/StbiLdrImageSaver.cpp
    include/tev/imageio/TiffImageLoader.h src/imageio/TiffImageLoader.cpp
    include/tev/imageio/UltraHdrImageLoader.h src/imageio/UltraHdrImageLoader.cpp
    include/tev/imageio/WebpImageLoader.h src/imageio/WebpImageLoader.cpp
    include/tev/imageio/Xmp.h src/imageio/Xmp.cpp

    include/tev/Box.h src/Box.cpp
    include/tev/Channel.h src/Channel.cpp
    include/tev/Common.h src/Common.cpp
    include/tev/FalseColor.h src/FalseColor.cpp
    include/tev/HelpWindow.h src/HelpWindow.cpp
    include/tev/ImageInfoWindow.h src/ImageInfoWindow.cpp
    include/tev/Image.h src/Image.cpp
    include/tev/ImageButton.h src/ImageButton.cpp
    include/tev/ImageCanvas.h src/ImageCanvas.cpp
    include/tev/ImageViewer.h src/ImageViewer.cpp
    include/tev/Ipc.h src/Ipc.cpp
    include/tev/Lazy.h src/Lazy.cpp
    include/tev/MultiGraph.h src/MultiGraph.cpp
    include/tev/SharedQueue.h src/SharedQueue.cpp
    include/tev/Task.h src/Task.cpp
    include/tev/ThreadPool.h src/ThreadPool.cpp
    include/tev/UberShader.h src/UberShader.cpp
    include/tev/VectorGraphics.h src/VectorGraphics.cpp
    include/tev/WaylandClipboard.h src/WaylandClipboard.cpp

    src/main.cpp
)
if (MSVC)
    list(APPEND TEV_SOURCES resources/windows/icon.rc)
    list(APPEND TEV_SOURCES resources/windows/tev.manifest)
    list(APPEND TEV_SOURCES include/tev/imageio/DdsImageLoader.h src/imageio/DdsImageLoader.cpp)
elseif (APPLE)
    list(APPEND TEV_SOURCES resources/macos/icon.icns)
endif()
if (TEV_USE_LIBHEIF)
    list(APPEND TEV_SOURCES include/tev/imageio/HeifImageLoader.h src/imageio/HeifImageLoader.cpp)
endif()
if (TEV_SUPPORT_JXL)
    list(APPEND TEV_SOURCES include/tev/imageio/JxlImageLoader.h src/imageio/JxlImageLoader.cpp)
    list(APPEND TEV_SOURCES include/tev/imageio/JxlImageSaver.h src/imageio/JxlImageSaver.cpp)
endif()

add_executable(tev ${TEV_SOURCES})

if (APPLE)
    set(RESOURCE_FILES
        resources/macos/icon.icns
    )

    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MACOSX_BUNDLE_MIN_OS_VERSION
            "<key>LSMinimumSystemVersion</key>\n    <string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>"
        )
    else()
        set(MACOSX_BUNDLE_MIN_OS_VERSION "")
    endif()

    set_target_properties(tev PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist"
        MACOSX_BUNDLE_BUNDLE_VERSION ${TEV_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${TEV_VERSION}
        MACOSX_BUNDLE_MIN_OS_VERSION "${MACOSX_BUNDLE_MIN_OS_VERSION}"
        RESOURCE "${RESOURCE_FILES}"
    )
endif()

target_include_directories(tev PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Header-only dependencies whose include path cannot be inherited from link targets
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/args
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/fmt/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/qoi
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/tinylogger
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/utfcpp/source
)

set(TEV_DEFINITIONS -DTEV_VERSION="${TEV_VERSION_ARCH}")
if (APPLE)
    # Starting with macOS Mojave OpenGL is deprecated. tev uses metal by default but can optionally be targeted at GL, so there is no point
    # in keeping the warnings around.
    list(APPEND TEV_DEFINITIONS -DGL_SILENCE_DEPRECATION)
endif()
if (TEV_SUPPORT_AVIF)
    list(APPEND TEV_DEFINITIONS -DTEV_SUPPORT_AVIF)
endif()
if (TEV_SUPPORT_HEIC)
    list(APPEND TEV_DEFINITIONS -DTEV_SUPPORT_HEIC)
endif()
if (TEV_USE_LIBHEIF)
    list(APPEND TEV_DEFINITIONS -DTEV_USE_LIBHEIF)
endif()
if (TEV_SUPPORT_JXL)
    list(APPEND TEV_DEFINITIONS -DTEV_SUPPORT_JXL)
endif()

target_compile_definitions(tev PRIVATE ${TEV_DEFINITIONS})

set(TEV_LIBS clip exif_static jpeg-turbo libraw::libraw_r nanogui OpenEXR::OpenEXR png_static tiff uhdr webpdemux XMPCore)
if (MSVC)
    list(APPEND TEV_LIBS DirectXTex wsock32 ws2_32)
endif()
if (TEV_USE_LIBHEIF)
    list(APPEND TEV_LIBS lcms2-tev heif)
endif()
if (TEV_SUPPORT_JXL)
    list(APPEND TEV_LIBS jxl jxl_dec jxl_threads)
endif()

target_link_libraries(tev ${TEV_LIBS})

# Custom compile options
if (MSVC)
    # Enable more warnings
    target_compile_options(tev PRIVATE /W4)

    # tev uses UTF-8 chars. Specify UTF-8 charset explicitly.
    # "/utf-8" is equivalent to "/source-charset:utf-8 /execution-charset:utf-8".
    target_compile_options(tev PRIVATE /utf-8)

    # To allow for wildcards in command-line path arguments on windows,
    # we need to link to wsetargv.obj
    # http://msdn.microsoft.com/en-us/library/8bch7bkk.aspx
    target_link_options(tev PRIVATE wsetargv.obj)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # Enable more warnings
    target_compile_options(tev PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(tev PRIVATE -Wno-gnu-anonymous-struct -Wno-c99-extensions -Wno-nested-anon-types -Wno-deprecated-register -Wno-deprecated-anon-enum-enum-conversion)
    endif()
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(tev PRIVATE -Wno-misleading-indentation -Wno-deprecated-declarations)
    endif()

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # Coroutines need to be explicitly enabled on g++
        target_compile_options(tev PRIVATE -fcoroutines)
    endif()
endif()

set(CPACK_PACKAGE_VENDOR "Tom94 (Thomas Müller)")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High dynamic range (HDR) image viewer for people who care about colors.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/tom94/tev")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if (APPLE)
    if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR CMAKE_INSTALL_PREFIX STREQUAL "")
        set(CMAKE_INSTALL_PREFIX "" CACHE PATH "" FORCE)
        install(CODE "
execute_process(COMMAND rm -f /usr/local/bin/tev)
execute_process(COMMAND ln -s /Applications/tev.app/Contents/MacOS/tev /usr/local/bin/tev)
        ")
    else()
        install(CODE "
execute_process(COMMAND mkdir -p \"${CMAKE_INSTALL_PREFIX}/bin\")
execute_process(COMMAND rm -f \"${CMAKE_INSTALL_PREFIX}/bin/tev\")
execute_process(COMMAND ln -s \"${CMAKE_INSTALL_PREFIX}/Applications/tev.app/Contents/MacOS/tev\" \"${CMAKE_INSTALL_PREFIX}/bin/tev\")
        ")
    endif()

    install(TARGETS tev BUNDLE DESTINATION "${CMAKE_INSTALL_PREFIX}/Applications")

    set(CPACK_GENERATOR "DragNDrop")
elseif (WIN32)
    install(TARGETS tev RUNTIME DESTINATION "bin")
    set(CPACK_GENERATOR "WIX")
    set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-installer")

    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/icon.png")
    set(CPACK_PACKAGE_EXECUTABLES "tev" "tev")

    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/patch.wxs")
    set(CPACK_WIX_UPGRADE_GUID "F3EC0D95-B2DC-4451-975C-CF591871E162")
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/icon.ico")

    configure_file(scripts/configure-winget.cmake scripts/configure-winget.cmake @ONLY)
    configure_file(resources/windows/winget.yaml resources/windows/winget.yaml COPYONLY)
    set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/scripts/configure-winget.cmake")
else()
    install(TARGETS tev)

    configure_file(resources/linux/tev.desktop resources/linux/tev.desktop)
    install(FILES ${CMAKE_BINARY_DIR}/resources/linux/tev.desktop DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications" RENAME "${TEV_LINUX_APP_ID}.desktop")

    if (TEV_CREATE_METAINFO)
        execute_process(
            COMMAND git for-each-ref "refs/tags/v*.*" --shell --sort=-creatordate --format [=[
                tag=%(refname:short)
                version="${tag#v}"
                printf '        <release version="%s" date="%s">\n            <url type="details">https://github.com/Tom94/tev/releases/tag/%s</url>\n        </release>\n' "$version" %(creatordate:short) "$tag"
            ]=]
            COMMAND sh
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE
            OUTPUT_VARIABLE TEV_METAINFO_RELEASES
        )

        configure_file(resources/linux/tev.metainfo.xml.in resources/linux/tev.metainfo.xml)
        install(FILES ${CMAKE_BINARY_DIR}/resources/linux/tev.metainfo.xml DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo" RENAME "${TEV_LINUX_APP_ID}.metainfo.xml")
    endif()

    if (TEV_VALIDATE_METAINFO)
        if (NOT TEV_CREATE_METAINFO)
            message(FATAL_ERROR "TEV_VALIDATE_METAINFO is set but TEV_CREATE_METAINFO is not set. Cannot validate non-existing metainfo.")
        endif()

        find_program(APPSTREAMCLI_EXECUTABLE appstreamcli)
        if (NOT APPSTREAMCLI_EXECUTABLE)
            message(FATAL_ERROR "appstreamcli not found -- cannot validate metainfo.")
        endif()

        add_custom_command(
            TARGET tev POST_BUILD
            COMMAND ${APPSTREAMCLI_EXECUTABLE} validate ${CMAKE_BINARY_DIR}/resources/linux/tev.metainfo.xml
        )
    endif()

    set(ICON_RESOLUTIONS "16;24;32;48;64;96;128;256;512")
    foreach(RES IN LISTS ICON_RESOLUTIONS)
        install(FILES resources/linux/icon-${RES}.png DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${RES}x${RES}/apps" RENAME "${TEV_LINUX_APP_ID}.png")
    endforeach()

    list(APPEND CPACK_GENERATOR External)
    set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create-appimage.cmake")
    set(CPACK_EXTERNAL_ENABLE_STAGING YES)
    set(CPACK_PACKAGE_FILE_NAME ${PROJECT_NAME})
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr) # requirement by AppDir specification
endif()

include(CPack)
