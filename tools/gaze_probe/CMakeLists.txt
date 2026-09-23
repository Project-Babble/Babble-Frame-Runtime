cmake_minimum_required(VERSION 3.16)
project(gaze_probe CXX)
# Standalone research tool: reads SteamVR eye-tracking input as a background
# OpenVR client. Not part of the runtime build.
set(CMAKE_CXX_STANDARD 17)
set(OPENVR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openvr)
add_executable(gaze_probe gaze_probe.cpp)
target_include_directories(gaze_probe SYSTEM PRIVATE ${OPENVR})
target_link_libraries(gaze_probe PRIVATE ${OPENVR}/lib/linuxarm64/libopenvr_api.so)
set_target_properties(gaze_probe PROPERTIES BUILD_RPATH ${OPENVR}/lib/linuxarm64)
