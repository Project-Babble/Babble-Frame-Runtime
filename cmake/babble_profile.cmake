# The build profile, and the config files derived from it.
#
#   dev      (default) everything: the Frame's own stereo mouth cameras ("frunk")
#            and the USB Babble tracker, every output, the dev tooling
#   release  the USB Babble tracker is the only mouth; VRCFaceTracking over the
#            PC link is the only output; no visualizer, no camera-stream switch;
#            only the models the pipelines load are packaged
#
# Sets BABBLE_FRUNK_MOUTH and BABBLE_DEV_FEATURES, and writes the profile's
# defaults.json, settingsschema.vrsettings.in (the @BABBLE_VERSION@ placeholder
# kept for cmake/babble_version.cmake) and default.vrsettings into the generated
# dir, by REMOVING entries from the single dev-profile sources:
#   runtime/resources/defaults.json
#   driver/package/resources/settings/settingsschema.vrsettings
#   driver/package/resources/settings/default.vrsettings
# so there is one source of truth. The dev profile applies no operation and
# writes the files verbatim. CMake's JSON writer re-serialises what it edits
# (keys sorted, floats at full precision); values are unchanged.
#
# To look at the result on a PC, without the rest of the build:
#   cmake -DBABBLE_PROFILE=release -DOUT_DIR=/tmp/profile -P cmake/babble_profile.cmake

if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED BABBLE_PROFILE)
        set(BABBLE_PROFILE dev)
    endif()
else()
    set(BABBLE_PROFILE "dev" CACHE STRING "Build profile: dev (everything) or release (USB mouth tracker only)")
    set_property(CACHE BABBLE_PROFILE PROPERTY STRINGS dev release)
endif()
if(BABBLE_PROFILE STREQUAL "dev")
    set(BABBLE_FRUNK_MOUTH ON)
    set(BABBLE_DEV_FEATURES ON)
elseif(BABBLE_PROFILE STREQUAL "release")
    set(BABBLE_FRUNK_MOUTH OFF)
    set(BABBLE_DEV_FEATURES OFF)
else()
    message(FATAL_ERROR "BABBLE_PROFILE=\"${BABBLE_PROFILE}\": use dev or release")
endif()
message(STATUS "Babble profile: ${BABBLE_PROFILE}")

# The index of the element of the JSON array at <path...> whose member <key>
# equals <value>. Fatal when there is none: an entry renamed or moved in the
# source file fails the configure, not the release.
function(babble_json_find out_var json key value)
    string(JSON n LENGTH "${json}" ${ARGN})
    math(EXPR last "${n} - 1")
    foreach(i RANGE 0 ${last})
        string(JSON v ERROR_VARIABLE err GET "${json}" ${ARGN} ${i} ${key})
        if(NOT err AND v STREQUAL value)
            set(${out_var} ${i} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "babble_profile: no element with \"${key}\": \"${value}\" in ${ARGN}")
endfunction()

# Removes that element from the array at <path...> of the JSON held in <var>.
function(babble_json_remove_where var key value)
    babble_json_find(i "${${var}}" ${key} ${value} ${ARGN})
    string(JSON ${var} REMOVE "${${var}}" ${ARGN} ${i})
    set(${var} "${${var}}" PARENT_SCOPE)
endfunction()

function(babble_profile_generate src_dir out_dir)
    set(res ${src_dir}/runtime/resources)
    set(settings ${src_dir}/driver/package/resources/settings)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${res}/defaults.json ${settings}/settingsschema.vrsettings ${settings}/default.vrsettings)
    endif()
    file(READ ${res}/defaults.json defaults)
    file(READ ${settings}/settingsschema.vrsettings schema)
    file(READ ${settings}/default.vrsettings vrdefaults)

    if(NOT BABBLE_FRUNK_MOUTH)
        # The Frame's own stereo mouth cameras: their source, pipeline and
        # capture threads, the camera arbiter, and the mouth source switch.
        string(JSON defaults REMOVE "${defaults}" mouth_source)
        string(JSON defaults REMOVE "${defaults}" sources mouth)
        string(JSON defaults REMOVE "${defaults}" pipelines mouth)
        string(JSON defaults REMOVE "${defaults}" arbiter)
        string(JSON defaults REMOVE "${defaults}" threads capL)
        string(JSON defaults REMOVE "${defaults}" threads capR)
        # SteamVR Settings: Mouth Tracking switches the USB tracker itself; no
        # source choice and no rate (the mono model runs at 30 Hz).
        babble_json_remove_where(defaults key mouthRate steamvr_settings values)
        babble_json_remove_where(defaults key mouthSource steamvr_settings values)
        babble_json_find(i "${defaults}" key mouthTracking steamvr_settings values)
        string(JSON defaults SET "${defaults}" steamvr_settings values ${i} id "\"/sources/face/tracking\"")
        babble_json_remove_where(defaults id /sources/mouth/process_fps dashboard settings)
        # The schema is one section ([0]) whose controls are its "values".
        babble_json_remove_where(schema name /settings/driver_babble/mouthRate 0 values)
        babble_json_remove_where(schema name /settings/driver_babble/mouthSource 0 values)
        babble_json_find(i "${schema}" name /settings/driver_babble/mouthTracking 0 values)
        string(JSON schema SET "${schema}" 0 values ${i} text
            "\"Mouth tracking from a USB Babble face tracker plugged into the headset. Pick the camera in the Babble app window's settings if more than one is connected.\"")
        string(JSON vrdefaults REMOVE "${vrdefaults}" driver_babble mouthSource)
        string(JSON vrdefaults REMOVE "${vrdefaults}" driver_babble mouthRate)
    endif()
    if(NOT BABBLE_DEV_FEATURES)
        # Release trims: VRCFaceTracking over the PC link is the only output
        # (no VRChat-native output, no local OSC for the visualizer), no
        # visualizer, no camera-stream switch, and only the models the eye
        # pipeline loads by default.
        string(JSON defaults REMOVE "${defaults}" output)
        string(JSON defaults REMOVE "${defaults}" sinks vrc_native)
        string(JSON defaults SET "${defaults}" sinks vrcft enabled false)
        string(JSON defaults REMOVE "${defaults}" visualizer)
        string(JSON defaults REMOVE "${defaults}" pipelines eye lid_model)
        string(JSON defaults REMOVE "${defaults}" pipelines eye pupil_model)
        babble_json_remove_where(defaults key output steamvr_settings values)
        babble_json_remove_where(defaults id /sinks/camera_stream/active dashboard settings)
        babble_json_remove_where(schema name /settings/driver_babble/output 0 values)
        string(JSON vrdefaults REMOVE "${vrdefaults}" driver_babble output)
    endif()

    file(WRITE ${out_dir}/defaults.json "${defaults}")
    file(WRITE ${out_dir}/settingsschema.vrsettings.in "${schema}")
    file(WRITE ${out_dir}/default.vrsettings "${vrdefaults}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT OUT_DIR)
        message(FATAL_ERROR "babble_profile: -DOUT_DIR=<dir> is required in script mode")
    endif()
    file(MAKE_DIRECTORY ${OUT_DIR})
    babble_profile_generate(${CMAKE_CURRENT_LIST_DIR}/.. ${OUT_DIR})
endif()
