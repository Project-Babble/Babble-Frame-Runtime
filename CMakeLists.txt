cmake_minimum_required(VERSION 3.16)
project(babble_frame_runtime CXX)

# Version: "V<feature/major>.<bugfix/incremental> <Alpha|Beta|RC|Release> [N]",
# e.g. "V0.1 Alpha" or "V0.1 Alpha 2" (the Nth build of that stage). Release
# builds pass it in (-DBABBLE_VERSION="V0.2 Beta 1"; hyphens instead of spaces,
# "V0.2-Beta-1", also work, for use as a git tag). Otherwise it comes from the
# VERSION file and the build is marked as a dev build with its commit.
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
string(REPLACE "-" " " babble_version "${babble_version}")   # the git-tag spelling
if(NOT babble_version MATCHES "^V[0-9]+\\.[0-9]+ (Alpha|Beta|RC|Release)( [0-9]+)?$")
    message(FATAL_ERROR "Version \"${babble_version}\" is not \"Vx.x Stage [N]\" "
                        "(Stage: Alpha, Beta, RC or Release; N optional, e.g. \"V0.1 Alpha 2\")")
endif()
message(STATUS "Babble version: ${babble_version}")

# Generates generated/babble_version.h and the versioned settings schema at
# every build (cmake/babble_version.cmake).
set(BABBLE_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(babble_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUT_DIR=${BABBLE_GENERATED_DIR}
            -DVERSION=${babble_version} -DDEV=${babble_dev} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/babble_version.cmake
    BYPRODUCTS ${BABBLE_GENERATED_DIR}/babble_version.h ${BABBLE_GENERATED_DIR}/settingsschema.vrsettings
    COMMENT "Version ${babble_version}"
    VERBATIM)
file(MAKE_DIRECTORY ${BABBLE_GENERATED_DIR})

# Builds the runtime daemon first, then the SteamVR driver folder that bundles it:
#   build/babble/driver.vrdrivermanifest
#   build/babble/bin/<platform>/driver_babble.so
#   build/babble/bin/<platform>/babble-runtime
#   build/babble/resources/...
add_subdirectory(runtime)
add_subdirectory(driver)
