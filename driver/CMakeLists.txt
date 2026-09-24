cmake_minimum_required(VERSION 3.16)
project(babble_driver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(BABBLE_PLATFORM linuxarm64)
else()
    set(BABBLE_PLATFORM linux64)
endif()

add_library(driver_babble SHARED src/driver_babble.cpp)
target_include_directories(driver_babble SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr)
target_compile_options(driver_babble PRIVATE -Wall -Wextra)
target_link_options(driver_babble PRIVATE -Wl,--no-undefined -static-libstdc++ -static-libgcc)
target_link_libraries(driver_babble PRIVATE dl pthread)
set_target_properties(driver_babble PROPERTIES PREFIX "")

# Assemble the SteamVR driver folder:
#   babble/driver.vrdrivermanifest
#   babble/resources/settings/default.vrsettings
#   babble/resources/settings/settingsschema.vrsettings  (the "Expression Tracking" section in SteamVR Settings)
#   babble/resources/runtime/defaults.json
#   babble/bin/<platform>/driver_babble.so
#   babble/bin/<platform>/babble-runtime
set(BABBLE_OUT ${CMAKE_BINARY_DIR}/babble)
set_target_properties(driver_babble PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${BABBLE_OUT}/bin/${BABBLE_PLATFORM})

# A separate target so relinking only the runtime still refreshes the folder.
add_custom_target(babble_package ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/package ${BABBLE_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/resources ${BABBLE_OUT}/resources/runtime
    # Copy then rename: the shim may be running the old binary, which cannot
    # be overwritten in place (ETXTBSY); rename swaps it atomically.
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:babble_runtime> ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/babble-runtime.new
    COMMAND ${CMAKE_COMMAND} -E rename
        ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/babble-runtime.new ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/babble-runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr/lib/linuxarm64/libopenvr_api.so
        ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/libopenvr_api.so
    DEPENDS driver_babble babble_runtime
    VERBATIM)
