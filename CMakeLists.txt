cmake_minimum_required(VERSION 3.24)
project(babble_frame_runtime CXX)

# Release builds set BABBLE_VERSION, and other builds use the VERSION file and are dev builds.
set(BABBLE_VERSION "" CACHE STRING "Release version, \"Vx.x Stage [N]\"; empty = VERSION file, dev build")
if(BABBLE_VERSION)
    set(babble_version "${BABBLE_VERSION}")
    set(babble_dev OFF)
else()
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION babble_version)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
    set(babble_dev ON)
endif()
string(STRIP "${babble_version}" babble_version)
string(REPLACE "-" " " babble_version "${babble_version}")   # Accept the git-tag format with hyphens.
if(NOT babble_version MATCHES "^V[0-9]+\\.[0-9]+ (Alpha|Beta|RC|Release)( [0-9]+)?$")
    message(FATAL_ERROR "Version \"${babble_version}\" is not \"Vx.x Stage [N]\" "
                        "(Stage: Alpha, Beta, RC or Release; N optional, e.g. \"V0.1 Alpha 2\")")
endif()
message(STATUS "Babble version: ${babble_version}")

set(BABBLE_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${BABBLE_GENERATED_DIR})

# Make the profile config files before add_subdirectory(runtime) because the runtime embeds defaults.json.
include(cmake/babble_profile.cmake)
babble_profile_generate(${CMAKE_CURRENT_SOURCE_DIR} ${BABBLE_GENERATED_DIR})

# Make babble_version.h and the versioned settings schema at every build.
add_custom_target(babble_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUT_DIR=${BABBLE_GENERATED_DIR}
            -DVERSION=${babble_version} -DDEV=${babble_dev} -DPROFILE=${BABBLE_PROFILE}
            -DSCHEMA_IN=${BABBLE_GENERATED_DIR}/settingsschema.vrsettings.in
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/babble_version.cmake
    BYPRODUCTS ${BABBLE_GENERATED_DIR}/babble_version.h ${BABBLE_GENERATED_DIR}/settingsschema.vrsettings
    COMMENT "Version ${babble_version} (${BABBLE_PROFILE} profile)"
    VERBATIM)

# Build the runtime daemon first because the SteamVR driver folder in build/babble contains it.
add_subdirectory(runtime)
add_subdirectory(driver)
