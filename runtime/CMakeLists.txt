cmake_minimum_required(VERSION 3.16)
project(babble_runtime CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BABBLE_ASAN "Build the runtime with AddressSanitizer (soak tests)" OFF)
option(BABBLE_TESTS "Build the runtime unit tests" ON)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# The shipped defaults are compiled into the binary so the runtime never
# depends on finding its resource folder.
set(BABBLE_DEFAULTS_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/defaults.json)
file(READ ${BABBLE_DEFAULTS_FILE} BABBLE_DEFAULTS_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BABBLE_DEFAULTS_FILE})
configure_file(src/core/defaults_json.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/defaults_json.h @ONLY)

# ExecuTorch runtime, built and installed once by tools/build_executorch.sh
# (XNNPACK backend + the selected portable kernels our models need).
set(BABBLE_EXECUTORCH_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../et-install" CACHE PATH
    "Install prefix of the ExecuTorch build (tools/build_executorch.sh)")
find_package(executorch CONFIG REQUIRED
    PATHS "${BABBLE_EXECUTORCH_PREFIX}/lib/cmake/ExecuTorch" NO_DEFAULT_PATH)

add_library(babble_core STATIC
    src/core/config.cpp
    src/core/lifecycle.cpp
    src/core/log.cpp
    src/core/metrics.cpp
    src/core/paths.cpp
    src/core/threading.cpp
    src/frame/frame_pool.cpp
    src/frame/stereo_pairer.cpp
    src/camera/v4l2/v4l2_capture.cpp
    src/camera/v4l2/v4l2_controls.cpp
    src/camera/v4l2/v4l2_subdev.cpp
    src/camera/mouth/exposure_controller.cpp
    src/camera/mouth/frame_mouth_source.cpp
    src/camera/eye/dsp_client.cpp
    src/camera/eye/dsp_eye_source.cpp
    src/camera/tracking_source.cpp
    src/platform/app_launcher.cpp
    src/platform/camera_arbiter_client.cpp
    src/platform/eye_server_client.cpp
    src/platform/media_graph.cpp
    src/platform/steamvr_client.cpp
    src/platform/steamvr_settings.cpp
    src/model/null_model_runner.cpp
    src/model/preprocess.cpp
    src/model/image_ops.cpp
    src/model/executorch_runner.cpp
    src/model/pipeline.cpp
    src/model/null_pipeline.cpp
    src/model/fusionnet_mouth.cpp
    src/model/babble_eye.cpp
    src/model/steamvr_gaze.cpp
    src/model/postprocess/mouth_post.cpp
    src/model/postprocess/eye_post.cpp
    src/state/expression_state.cpp
    src/sink/debug_sink.cpp
    src/sink/sink.cpp
    src/sink/osc.cpp
    src/sink/osc_sink.cpp
    src/ui/canvas.cpp
    src/ui/font.cpp
    src/ui/dashboard_view.cpp
    src/ui/dashboard.cpp
    src/ui/vk_uploader.cpp
)
target_include_directories(babble_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated
    SYSTEM PUBLIC ${THIRD_PARTY}/nlohmann_json/single_include ${THIRD_PARTY}/openvr ${THIRD_PARTY}/stb)
target_compile_options(babble_core PUBLIC -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(babble_core PUBLIC pthread
    executorch extension_module_static extension_tensor extension_threadpool
    xnnpack_backend executorch_selected_kernels portable_kernels
    ${THIRD_PARTY}/openvr/lib/linuxarm64/libopenvr_api.so
    vulkan)

if(BABBLE_ASAN)
    target_compile_options(babble_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(babble_core PUBLIC -fsanitize=address)
endif()

add_executable(babble_runtime src/main.cpp)
target_link_libraries(babble_runtime PRIVATE babble_core)
# libopenvr_api.so (SteamVR client, for eye gaze) ships next to the binary.
set_target_properties(babble_runtime PROPERTIES OUTPUT_NAME babble-runtime
    BUILD_RPATH "${THIRD_PARTY}/openvr/lib/linuxarm64" INSTALL_RPATH "$ORIGIN")
target_link_options(babble_runtime PRIVATE "-Wl,-rpath,$ORIGIN")
if(NOT BABBLE_ASAN)
    # Ships inside a Steam depot: no dependency on the OS's libstdc++.
    target_link_options(babble_runtime PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Development tool: renders the dashboard pages to PNG without SteamVR.
add_executable(dashboard_preview tools/dashboard_preview.cpp)
target_link_libraries(dashboard_preview PRIVATE babble_core)
set_target_properties(dashboard_preview PROPERTIES BUILD_RPATH "${THIRD_PARTY}/openvr/lib/linuxarm64")
target_compile_definitions(dashboard_preview PRIVATE BABBLE_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")

if(BABBLE_TESTS)
    enable_testing()
    add_executable(babble_tests
        tests/main.cpp
        tests/test_mailbox.cpp
        tests/test_stereo_pairer.cpp
        tests/test_rate_limiter.cpp
        tests/test_dsp_layout.cpp
        tests/test_config.cpp
        tests/test_tracking_source.cpp
        tests/test_sinks.cpp
        tests/test_image_ops.cpp
        tests/test_executorch_runner.cpp
        tests/test_postprocess.cpp
        tests/test_gaze.cpp
        tests/test_dashboard.cpp
        tests/test_eye_server.cpp
    )
    target_link_libraries(babble_tests PRIVATE babble_core)
    set_target_properties(babble_tests PROPERTIES BUILD_RPATH "${THIRD_PARTY}/openvr/lib/linuxarm64")
    target_include_directories(babble_tests SYSTEM PRIVATE ${THIRD_PARTY}/doctest)
    target_compile_definitions(babble_tests PRIVATE BABBLE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        BABBLE_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/models"
        BABBLE_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    add_test(NAME babble_tests COMMAND babble_tests)
endif()
