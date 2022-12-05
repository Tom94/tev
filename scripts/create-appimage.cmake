include(CMakePrintHelpers)
cmake_print_variables(CPACK_TEMPORARY_DIRECTORY)
cmake_print_variables(CPACK_TOPLEVEL_DIRECTORY)
cmake_print_variables(CPACK_PACKAGE_DIRECTORY)
cmake_print_variables(CPACK_PACKAGE_FILE_NAME)
cmake_print_variables(CMAKE_SYSTEM_PROCESSOR)
cmake_print_variables(PROJECT_BINARY_DIR)

find_program(LINUXDEPLOY_EXECUTABLE
    NAMES linuxdeploy linuxdeploy-${CMAKE_SYSTEM_PROCESSOR}.AppImage
    PATHS ${CPACK_PACKAGE_DIRECTORY}/dependencies/)

if (NOT LINUXDEPLOY_EXECUTABLE)
    message(Warning "Couldn't build linuxdeploy. Downloading pre-build binary instead.")
    set(LINUXDEPLOY_EXECUTABLE ${CPACK_PACKAGE_DIRECTORY}/dependencies/linuxdeploy-${CMAKE_SYSTEM_PROCESSOR}.AppImage)
    file(DOWNLOAD 
        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${CMAKE_SYSTEM_PROCESSOR}.AppImage
        ${LINUXDEPLOY_EXECUTABLE}
        INACTIVITY_TIMEOUT 10
        LOG ${CPACK_PACKAGE_DIRECTORY}/linuxdeploy/download.log
        STATUS LINUXDEPLOY_DOWNLOAD)
    execute_process(COMMAND chmod +x ${LINUXDEPLOY_EXECUTABLE} COMMAND_ECHO STDOUT)
endif()

execute_process(
COMMAND
    ${CMAKE_COMMAND} -E env
    OUTPUT=${CPACK_PACKAGE_FILE_NAME}.appimage
    VERSION=${CPACK_PACKAGE_VERSION}
    ${LINUXDEPLOY_EXECUTABLE}
    --appdir=${CPACK_TEMPORARY_DIRECTORY}
    --output=appimage
    #    --verbosity=2
)