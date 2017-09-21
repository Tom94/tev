# Some components of this file were taken from PBRT-v3 (BSD 2-clause "Simplified" License)
cmake_minimum_required(VERSION 2.8.3)

project(tev)
set(VERSION "1.4.dev")

# Set ourselves as the startup project in visual studio.
# Not available until cmake 3.6, but doesn't break older versions.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT tev)

# Set a default configuration if none was specified
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No release type specified. Setting to 'Release'.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
endif()

set(CMAKE_CXX_STANDARD 11)

if (APPLE)
    set(CMAKE_MACOSX_RPATH ON)
endif()

if (MSVC)
    # Disable annoying secure CRT warnings
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_CRT_SECURE_NO_WARNINGS")

    # Parallel build
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")

    # 32 bit windows
    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /DEIGEN_DONT_ALIGN")
    endif()

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
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/dependencies)

# Enable more warnings
if (MSVC)
    if (CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    endif()

    # To allow for wildcards in command-line path arguments on windows,
    # we need to link to setargv.obj
    # http://msdn.microsoft.com/en-us/library/8bch7bkk.aspx
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} setargv.obj")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-gnu-anonymous-struct -Wno-c99-extensions -Wno-nested-anon-types -Wno-deprecated-register")
    endif()
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-misleading-indentation -Wno-deprecated-declarations")
    endif()
endif()

set(TEV_LIBS IlmImf nanogui ${NANOGUI_EXTRA_LIBS})
if (MSVC)
    set(TEV_LIBS ${TEV_LIBS} zlibstatic wsock32 ws2_32)
endif()

set(TEV_SOURCES
    include/tev/Channel.h src/Channel.cpp
    include/tev/Common.h src/Common.cpp
    include/tev/FalseColor.h src/FalseColor.cpp
    include/tev/GlTexture.h src/GlTexture.cpp
    include/tev/HelpWindow.h src/HelpWindow.cpp
    include/tev/Image.h src/Image.cpp
    include/tev/ImageButton.h src/ImageButton.cpp
    include/tev/ImageCanvas.h src/ImageCanvas.cpp
    include/tev/ImageViewer.h src/ImageViewer.cpp
    include/tev/Ipc.h src/Ipc.cpp
    include/tev/Lazy.h src/Lazy.cpp
    include/tev/SharedQueue.h src/SharedQueue.cpp
    include/tev/ThreadPool.h src/ThreadPool.cpp
    include/tev/UberShader.h src/UberShader.cpp
    src/main.cpp
)
if (MSVC)
    set(TEV_SOURCES ${TEV_SOURCES} resources/icon.rc)
elseif (APPLE)
    set(TEV_SOURCES ${TEV_SOURCES} resources/icon.icns scripts/osx-run-tev.sh)
endif()

add_executable(tev ${TEV_SOURCES})

if (APPLE)
    set_target_properties(tev PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist"
        MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${VERSION}
        MACOSX_BUNDLE_LONG_VERSION_STRING ${VERSION}
        RESOURCE resources/icon.icns
        RESOURCE scripts/osx-run-tev.sh
    )
endif()

include_directories(
    ${ARGS_INCLUDE}
    ${EIGEN_INCLUDE}
    ${GLFW_INCLUDE}
    ${NANOGUI_EXTRA_INCS}
    ${NANOGUI_INCLUDE}
    ${OPENEXR_INCLUDE_DIRS}
    ${STB_INCLUDE}
    ${TINYFORMAT_INCLUDE}
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_definitions(${NANOGUI_EXTRA_DEFS})

target_link_libraries(tev ${TEV_LIBS})

if (APPLE)
    install(TARGETS tev BUNDLE DESTINATION "/Applications")
    install(SCRIPT scripts/osx-post-install.cmake)
elseif(WIN32)
    install(TARGETS tev RUNTIME DESTINATION "bin")
else()
    install(TARGETS tev RUNTIME DESTINATION "bin")
    install(FILES resources/tev.desktop DESTINATION "/usr/share/applications")
    install(FILES resources/icon-512.png DESTINATION "/usr/share/icons/hicolor/512x512/apps" RENAME tev.png)
    install(FILES resources/icon-256.png DESTINATION "/usr/share/icons/hicolor/256x256/apps" RENAME tev.png)
    install(FILES resources/icon-128.png DESTINATION "/usr/share/icons/hicolor/128x128/apps" RENAME tev.png)
    install(FILES resources/icon-96.png DESTINATION "/usr/share/icons/hicolor/96x96/apps" RENAME tev.png)
    install(FILES resources/icon-64.png DESTINATION "/usr/share/icons/hicolor/64x64/apps" RENAME tev.png)
    install(FILES resources/icon-48.png DESTINATION "/usr/share/icons/hicolor/48x48/apps" RENAME tev.png)
    install(FILES resources/icon-32.png DESTINATION "/usr/share/icons/hicolor/32x32/apps" RENAME tev.png)
    install(FILES resources/icon-24.png DESTINATION "/usr/share/icons/hicolor/24x24/apps" RENAME tev.png)
    install(FILES resources/icon-16.png DESTINATION "/usr/share/icons/hicolor/16x16/apps" RENAME tev.png)
endif()
