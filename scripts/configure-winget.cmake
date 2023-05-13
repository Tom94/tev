include(CMakePrintHelpers)
cmake_print_variables(CPACK_TEMPORARY_DIRECTORY)
cmake_print_variables(CPACK_TOPLEVEL_DIRECTORY)
cmake_print_variables(CPACK_PACKAGE_DIRECTORY)
cmake_print_variables(CPACK_PACKAGE_FILE_NAME)
cmake_print_variables(CMAKE_SYSTEM_PROCESSOR)
cmake_print_variables(CPACK_INSTALL_CMAKE_PROJECTS)

set(TEV_VERSION "@TEV_VERSION@")
list(GET CPACK_INSTALL_CMAKE_PROJECTS 0 TEV_BUILD_DIR)

file(SHA256 "${CPACK_PACKAGE_FILES}" TEV_INSTALLER_SHA256)
configure_file("${TEV_BUILD_DIR}/resources/winget.yaml" "${TEV_BUILD_DIR}/tev.yaml")
