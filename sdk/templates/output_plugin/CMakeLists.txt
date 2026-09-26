# Template output plugin for the Babble Frame Runtime. Copy this directory,
# rename the target and the plugin name in plugin.cpp, and build:
#
#   cmake -S . -B build -DBABBLE_SDK_DIR=<runtime repo>/sdk && cmake --build build
#
# The result, build/libbabble_udp_json.so, goes into the driver's
# bin/linuxarm64/plugins/ (or pass its path as "library" in the config).
cmake_minimum_required(VERSION 3.16)
project(babble_udp_json CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The ABI header: this template sits in the runtime repo's sdk/ by default.
set(BABBLE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to the runtime repo's sdk/ directory")

add_library(babble_udp_json SHARED plugin.cpp)
target_include_directories(babble_udp_json PRIVATE ${BABBLE_SDK_DIR}/include)
target_compile_options(babble_udp_json PRIVATE -Wall -Wextra -fvisibility=hidden)
# Ships inside a Steam depot next to the runtime: carry our own C++ runtime,
# and fail at link time (not at load time) on missing symbols.
target_link_options(babble_udp_json PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,--exclude-libs,ALL)
find_package(Threads REQUIRED)
target_link_libraries(babble_udp_json PRIVATE Threads::Threads)
