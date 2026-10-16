cmake_minimum_required(VERSION 3.16)
project(overlay_probe CXX)
# This research tool is not part of the runtime build, and it tests a SteamVR dashboard overlay on the Frame.
set(CMAKE_CXX_STANDARD 17)
set(OPENVR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openvr)
add_executable(overlay_probe overlay_probe.cpp)
target_include_directories(overlay_probe SYSTEM PRIVATE ${OPENVR})
target_link_libraries(overlay_probe PRIVATE vulkan ${OPENVR}/lib/linuxarm64/libopenvr_api.so)
set_target_properties(overlay_probe PROPERTIES BUILD_RPATH ${OPENVR}/lib/linuxarm64)
