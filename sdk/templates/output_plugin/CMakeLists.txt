# Copy this template output plugin, change the target and plugin names, and build it with -DBABBLE_SDK_DIR=<repo>/sdk.
cmake_minimum_required(VERSION 3.16)
project(babble_udp_json CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The default path is correct when this template is in the sdk/ directory of the runtime repo.
set(BABBLE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to the runtime repo's sdk/ directory")

add_library(babble_udp_json SHARED plugin.cpp)
target_include_directories(babble_udp_json PRIVATE ${BABBLE_SDK_DIR}/include)
target_compile_options(babble_udp_json PRIVATE -Wall -Wextra -fvisibility=hidden)
# Link the C++ runtime statically, and make missing symbols fail at link time and not at load time.
target_link_options(babble_udp_json PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,--exclude-libs,ALL)
find_package(Threads REQUIRED)
target_link_libraries(babble_udp_json PRIVATE Threads::Threads)
