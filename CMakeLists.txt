# Some components of this file were taken from PBRT-v3 (BSD 2-clause "Simplified" License)
# PBRT-v3 can be found here: https://github.com/mmp/pbrt-v3
cmake_minimum_required(VERSION 3.13)

project(
    tev
    VERSION 1.25
    DESCRIPTION "High dynamic range (HDR) image comparison tool for graphics people. With an emphasis on OpenEXR images."
    LANGUAGES C CXX
)
set(TEV_VERSION "${CMAKE_PROJECT_VERSION}")

if (NOT TEV_DEPLOY)
    set(TEV_VERSION "${TEV_VERSION}dev")
endif()

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TEV_VERSION_ARCH "${TEV_VERSION} (64 bit)")
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(TEV_VERSION_ARCH "${TEV_VERSION} (32 bit)")
endif()

# Set ourselves as the startup project in visual studio.
# Not available until cmake 3.6, but doesn't break older versions.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT tev)

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
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    if (${CMAKE_SYSTEM_NAME} MATCHES "Emscripten")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s USE_ZLIB=1 -s DISABLE_EXCEPTION_CATCHING=0")
    endif()
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

add_compile_options(${NANOGUI_NATIVE_FLAGS})

# Enable more warnings
if (MSVC)
    if (CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    endif()

    # tev uses UTF-8 chars. Specify UTF-8 charset explicitly.
    # "/utf-8" is equivalent to "/source-charset:utf-8 /execution-charset:utf-8".
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    # Disable warnings that are present in dependencies and irrelevant to us
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4100") # unused arguments
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd5054") # deprecated enum & operator

    # To allow for wildcards in command-line path arguments on windows,
    # we need to link to wsetargv.obj
    # http://msdn.microsoft.com/en-us/library/8bch7bkk.aspx
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} wsetargv.obj")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-gnu-anonymous-struct -Wno-c99-extensions -Wno-nested-anon-types -Wno-deprecated-register -Wno-deprecated-anon-enum-enum-conversion")
    endif()
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-misleading-indentation -Wno-deprecated-declarations")
    endif()
endif()

# Coroutines need to be explicitly enabled on g++
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

set(TEV_LIBS clip IlmImf nanogui ${NANOGUI_EXTRA_LIBS})
if (MSVC)
    set(TEV_LIBS ${TEV_LIBS} zlibstatic DirectXTex wsock32 ws2_32)
endif()

set(TEV_SOURCES
    include/tev/imageio/ClipboardImageLoader.h src/imageio/ClipboardImageLoader.cpp
    include/tev/imageio/EmptyImageLoader.h src/imageio/EmptyImageLoader.cpp
    include/tev/imageio/ExrImageLoader.h src/imageio/ExrImageLoader.cpp
    include/tev/imageio/ExrImageSaver.h src/imageio/ExrImageSaver.cpp
    include/tev/imageio/ImageLoader.h src/imageio/ImageLoader.cpp
    include/tev/imageio/ImageSaver.h src/imageio/ImageSaver.cpp
    include/tev/imageio/PfmImageLoader.h src/imageio/PfmImageLoader.cpp
    include/tev/imageio/QoiImageLoader.h src/imageio/QoiImageLoader.cpp
    include/tev/imageio/QoiImageSaver.h src/imageio/QoiImageSaver.cpp
    include/tev/imageio/StbiHdrImageSaver.h src/imageio/StbiHdrImageSaver.cpp
    include/tev/imageio/StbiImageLoader.h src/imageio/StbiImageLoader.cpp
    include/tev/imageio/StbiLdrImageSaver.h src/imageio/StbiLdrImageSaver.cpp

    include/tev/Box.h src/Box.cpp
    include/tev/Channel.h src/Channel.cpp
    include/tev/Common.h src/Common.cpp
    include/tev/FalseColor.h src/FalseColor.cpp
    include/tev/HelpWindow.h src/HelpWindow.cpp
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

    src/main.cpp
)
if (MSVC)
    set(TEV_SOURCES ${TEV_SOURCES} resources/icon.rc)
    set(TEV_SOURCES ${TEV_SOURCES} resources/tev.manifest)
    set(TEV_SOURCES ${TEV_SOURCES} include/tev/imageio/DdsImageLoader.h src/imageio/DdsImageLoader.cpp)
elseif (APPLE)
    set(TEV_SOURCES ${TEV_SOURCES} resources/icon.icns scripts/mac-run-tev.sh)
endif()

add_executable(tev ${TEV_SOURCES})

if (APPLE)
    set(RESOURCE_FILES
        resources/icon.icns
        scripts/mac-run-tev.sh
    )

    set_target_properties(tev PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist"
        MACOSX_BUNDLE_BUNDLE_VERSION ${TEV_VERSION_ARCH}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${TEV_VERSION}
        MACOSX_BUNDLE_LONG_VERSION_STRING ${TEV_VERSION_ARCH}
        RESOURCE "${RESOURCE_FILES}"
    )
endif()

include_directories(
    ${ARGS_INCLUDE}
    ${CLIP_INCLUDE}
    ${DIRECTXTEX_INCLUDE}
    ${FMT_INCLUDE}
    ${GLFW_INCLUDE}
    ${NANOGUI_EXTRA_INCS}
    ${NANOGUI_INCLUDE}
    ${OPENEXR_INCLUDE_DIRS}
    ${QOI_INCLUDE}
    ${STB_INCLUDE}
    ${TINYLOGGER_INCLUDE}
    ${UTFCPP_INCLUDE}
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set(TEV_DEFINITIONS -DTEV_VERSION="${TEV_VERSION_ARCH}")
if (APPLE)
    # Starting with macOS Mojave OpenGL is deprecated. tev will switch
    # away from OpenGL once the underlying nanogui does, so there is no
    # point in keeping the warnings around.
    set(TEV_DEFINITIONS ${TEV_DEFINITIONS} -DGL_SILENCE_DEPRECATION)
endif()

add_definitions(${TEV_DEFINITIONS} ${NANOGUI_EXTRA_DEFS})

target_link_libraries(tev ${TEV_LIBS})

if (APPLE)
    install(TARGETS tev BUNDLE DESTINATION "/Applications")
    install(SCRIPT scripts/mac-post-install.cmake)
elseif (WIN32)
    install(TARGETS tev RUNTIME DESTINATION "bin")
else()
    install(TARGETS tev)

    configure_file(resources/tev.desktop resources/tev.desktop)
    install(FILES ${CMAKE_BINARY_DIR}/resources/tev.desktop DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")

    set(ICON_RESOLUTIONS "16;24;32;48;64;96;128;256;512")
    foreach(RES IN LISTS ICON_RESOLUTIONS)
        install(FILES resources/icon-${RES}.png DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${RES}x${RES}/apps" RENAME tev.png)
    endforeach()

    list(APPEND CPACK_GENERATOR External)
    set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/create-appimage.cmake")
    set(CPACK_EXTERNAL_ENABLE_STAGING YES)
    set(CPACK_PACKAGE_FILE_NAME ${PROJECT_NAME})
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr) # requirement by AppDir specification
    include(CPack)
endif()
