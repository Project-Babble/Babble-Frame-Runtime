cmake_minimum_required(VERSION 3.16)
project(gaze_probe CXX)
# This research tool is not part of the runtime build, and it reads SteamVR eye tracking as a background client.
set(CMAKE_CXX_STANDARD 17)
set(OPENVR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openvr)
add_executable(gaze_probe gaze_probe.cpp)
target_include_directories(gaze_probe SYSTEM PRIVATE ${OPENVR})
target_link_libraries(gaze_probe PRIVATE ${OPENVR}/lib/linuxarm64/libopenvr_api.so)
set_target_properties(gaze_probe PROPERTIES BUILD_RPATH ${OPENVR}/lib/linuxarm64)
