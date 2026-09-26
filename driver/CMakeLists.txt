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
target_include_directories(driver_babble SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann_json/single_include)
target_compile_options(driver_babble PRIVATE -Wall -Wextra)
target_link_options(driver_babble PRIVATE -Wl,--no-undefined -static-libstdc++ -static-libgcc)
target_link_libraries(driver_babble PRIVATE dl pthread)
set_target_properties(driver_babble PROPERTIES PREFIX "")

# babble-setup: the Steam launch target. Registers the driver folder with
# SteamVR and restarts SteamVR so it loads (see src/babble_setup.cpp).
add_executable(babble_setup src/babble_setup.cpp)
target_include_directories(babble_setup SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann_json/single_include)
target_compile_options(babble_setup PRIVATE -Wall -Wextra)
target_link_options(babble_setup PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(babble_setup PROPERTIES OUTPUT_NAME babble-setup)

# Assemble the SteamVR driver folder:
#   babble/driver.vrdrivermanifest
#   babble/resources/settings/default.vrsettings
#   babble/resources/settings/settingsschema.vrsettings  (the "Expression Tracking" section in SteamVR Settings)
#   babble/resources/runtime/defaults.json
#   babble/installscript.vdf   (Steam runs babble-setup --uninstall before deleting the folder)
#   babble/bin/<platform>/driver_babble.so
#   babble/bin/<platform>/babble-runtime
#   babble/bin/<platform>/babble-setup
set(BABBLE_OUT ${CMAKE_BINARY_DIR}/babble)
set_target_properties(driver_babble PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${BABBLE_OUT}/bin/${BABBLE_PLATFORM})
set_target_properties(babble_setup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BABBLE_OUT}/bin/${BABBLE_PLATFORM})

# Output plugins built elsewhere, as a list of .so
# paths; each is copied into bin/<platform>/plugins/, where a bare
# "library" name in the config finds it.
set(BABBLE_PLUGINS "" CACHE STRING "Output plugin libraries (.so paths, ;-separated) to package")
set(BABBLE_PLUGIN_COMMANDS)
foreach(plugin IN LISTS BABBLE_PLUGINS)
    get_filename_component(plugin_name ${plugin} NAME)
    set(dst ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/plugins/${plugin_name})
    # Copy then rename, like the runtime: a running runtime has the old one mapped.
    list(APPEND BABBLE_PLUGIN_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/plugins
        COMMAND ${CMAKE_COMMAND} -E copy ${plugin} ${dst}.new
        COMMAND ${CMAKE_COMMAND} -E rename ${dst}.new ${dst})
endforeach()

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
    ${BABBLE_PLUGIN_COMMANDS}
    DEPENDS driver_babble babble_runtime babble_setup
    VERBATIM)
