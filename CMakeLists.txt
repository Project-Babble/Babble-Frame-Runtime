cmake_minimum_required(VERSION 3.16)
project(babble_frame_runtime CXX)

# Builds the runtime daemon first, then the SteamVR driver folder that bundles it:
#   build/babble/driver.vrdrivermanifest
#   build/babble/bin/<platform>/driver_babble.so
#   build/babble/bin/<platform>/babble-runtime
#   build/babble/resources/...
add_subdirectory(runtime)
add_subdirectory(driver)
