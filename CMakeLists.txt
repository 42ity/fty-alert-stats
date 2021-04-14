cmake_minimum_required(VERSION 3.13)
cmake_policy(VERSION 3.13)

##############################################################################################################

project(fty-alert-stats VERSION 1.0.0)

########################################################################################################################

find_package(fty-cmake PATHS ${CMAKE_BINARY_DIR})

########################################################################################################################

add_subdirectory(lib)
add_subdirectory(agent)

## agent configuration
## https://cmake.org/cmake/help/v3.0/module/GNUInstallDirs.html

set(AGENT_USER "bios")
set(AGENT_ETC_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/${PROJECT_NAME}")
set(AGENT_VAR_DIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/fty/${PROJECT_NAME}")

# var/lib/fty runtime directory
install(DIRECTORY DESTINATION ${AGENT_VAR_DIR})

# cfg file -> etc/
configure_file("${PROJECT_SOURCE_DIR}/resources/fty-alert-stats.cfg.in" "${PROJECT_BINARY_DIR}/resources/fty-alert-stats.cfg" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/resources/fty-alert-stats.cfg" DESTINATION ${AGENT_ETC_DIR})

# service -> lib/systemd/system
configure_file("${PROJECT_SOURCE_DIR}/resources/fty-alert-stats.service.in" "${PROJECT_BINARY_DIR}/resources/fty-alert-stats.service" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/resources/fty-alert-stats.service" DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system/)
