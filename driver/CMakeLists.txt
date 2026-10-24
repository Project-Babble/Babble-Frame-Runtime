cmake_minimum_required(VERSION 3.24)
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
target_include_directories(driver_babble PRIVATE ${BABBLE_GENERATED_DIR})   # This directory has babble_version.h.
add_dependencies(driver_babble babble_version)
target_compile_options(driver_babble PRIVATE -Wall -Wextra)
target_link_options(driver_babble PRIVATE -Wl,--no-undefined -static-libstdc++ -static-libgcc
    $<$<CONFIG:Release>:-Wl,--strip-all>)
target_link_libraries(driver_babble PRIVATE dl pthread)
set_target_properties(driver_babble PROPERTIES PREFIX "")

# babble-setup is the Steam launch target that registers the driver folder with SteamVR.
add_executable(babble_setup src/babble_setup.cpp)
target_include_directories(babble_setup SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann_json/single_include)
target_compile_options(babble_setup PRIVATE -Wall -Wextra)
target_link_options(babble_setup PRIVATE -static-libstdc++ -static-libgcc
    $<$<CONFIG:Release>:-Wl,--strip-all>)
set_target_properties(babble_setup PROPERTIES OUTPUT_NAME babble-setup)

# Put all the parts of the SteamVR driver folder in build/babble.
set(BABBLE_OUT ${CMAKE_BINARY_DIR}/babble)
set_target_properties(driver_babble PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${BABBLE_OUT}/bin/${BABBLE_PLATFORM})
set_target_properties(babble_setup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BABBLE_OUT}/bin/${BABBLE_PLATFORM})

# Copy each output plugin .so into bin/<platform>/plugins/, where the config can find it by its file name.
set(BABBLE_PLUGINS "" CACHE STRING "Output plugin libraries (.so paths, ;-separated) to package")
set(BABBLE_PLUGIN_COMMANDS)
foreach(plugin IN LISTS BABBLE_PLUGINS)
    get_filename_component(plugin_name ${plugin} NAME)
    set(dst ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/plugins/${plugin_name})
    # Copy and then rename because a running runtime has the old plugin mapped.
    list(APPEND BABBLE_PLUGIN_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/plugins
        COMMAND ${CMAKE_COMMAND} -E copy ${plugin} ${dst}.new
        COMMAND ${CMAKE_COMMAND} -E rename ${dst}.new ${dst})
endforeach()

# Package only the runtime resources and models that the pipelines of the profile load.
set(runtime_res ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/resources)
set(BABBLE_PACKAGED_MODELS eye_kit_stage1_fp16.pte eye_kit_stage2_fp16.pte face_mono_224_fp16.pte)
set(BABBLE_PACKAGED_FILES)
if(BABBLE_FRUNK_MOUTH)
    list(APPEND BABBLE_PACKAGED_MODELS fusionnet_int8_xnnpack_qat_linear.pte)
    list(APPEND BABBLE_PACKAGED_FILES calibration_mouth.json)
endif()
set(BABBLE_RESOURCE_COMMANDS
    # Remove the old folder so that no file from a different profile stays.
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${BABBLE_OUT}/resources/runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BABBLE_OUT}/resources/runtime/models
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${runtime_res}/fonts ${BABBLE_OUT}/resources/runtime/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${runtime_res}/icons ${BABBLE_OUT}/resources/runtime/icons
    COMMAND ${CMAKE_COMMAND} -E copy ${BABBLE_GENERATED_DIR}/defaults.json ${BABBLE_OUT}/resources/runtime/defaults.json)
foreach(f IN LISTS BABBLE_PACKAGED_FILES)
    list(APPEND BABBLE_RESOURCE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy ${runtime_res}/${f} ${BABBLE_OUT}/resources/runtime/${f})
endforeach()
foreach(m IN LISTS BABBLE_PACKAGED_MODELS)
    list(APPEND BABBLE_RESOURCE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy ${runtime_res}/models/${m} ${BABBLE_OUT}/resources/runtime/models/${m})
endforeach()

# Use a separate target so that a relink of only the runtime also updates the folder.
add_custom_target(babble_package ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/package ${BABBLE_OUT}
    # Replace the dev SteamVR defaults from package/ with the defaults of the profile.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${BABBLE_GENERATED_DIR}/default.vrsettings ${BABBLE_OUT}/resources/settings/default.vrsettings
    # Copy the settings schema that has the version in it.
    COMMAND ${CMAKE_COMMAND} -E copy
        ${BABBLE_GENERATED_DIR}/settingsschema.vrsettings ${BABBLE_OUT}/resources/settings/settingsschema.vrsettings
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE ${BABBLE_OUT}/LICENSE
    ${BABBLE_RESOURCE_COMMANDS}
    # Copy and then rename because the system cannot overwrite a running binary (ETXTBSY).
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:babble_runtime> ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/babble-runtime.new
    COMMAND ${CMAKE_COMMAND} -E rename
        ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/babble-runtime.new ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/babble-runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr/lib/linuxarm64/libopenvr_api.so
        ${BABBLE_OUT}/bin/${BABBLE_PLATFORM}/libopenvr_api.so
    ${BABBLE_PLUGIN_COMMANDS}
    DEPENDS driver_babble babble_runtime babble_setup babble_version
    VERBATIM)
