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
    src/platform/camera_arbiter_client.cpp
    src/platform/media_graph.cpp
    src/model/null_model_runner.cpp
    src/model/preprocess.cpp
    src/model/pipeline.cpp
    src/state/expression_state.cpp
    src/sink/debug_sink.cpp
)
target_include_directories(babble_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated
    SYSTEM PUBLIC ${THIRD_PARTY}/nlohmann_json/single_include)
target_compile_options(babble_core PUBLIC -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(babble_core PUBLIC pthread)

if(BABBLE_ASAN)
    target_compile_options(babble_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(babble_core PUBLIC -fsanitize=address)
endif()

add_executable(babble_runtime src/main.cpp)
target_link_libraries(babble_runtime PRIVATE babble_core)
set_target_properties(babble_runtime PROPERTIES OUTPUT_NAME babble-runtime)
if(NOT BABBLE_ASAN)
    # Ships inside a Steam depot: no dependency on the OS's libstdc++.
    target_link_options(babble_runtime PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(BABBLE_TESTS)
    enable_testing()
    add_executable(babble_tests
        tests/main.cpp
        tests/test_mailbox.cpp
        tests/test_stereo_pairer.cpp
        tests/test_rate_limiter.cpp
        tests/test_dsp_layout.cpp
        tests/test_config.cpp
    )
    target_link_libraries(babble_tests PRIVATE babble_core)
    target_include_directories(babble_tests SYSTEM PRIVATE ${THIRD_PARTY}/doctest)
    add_test(NAME babble_tests COMMAND babble_tests)
endif()
