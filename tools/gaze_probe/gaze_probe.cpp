// gaze_probe: can a background OpenVR client read SteamVR's eye tracking?
//
//   gaze_probe [seconds] [binding-path]      (default 10 s, "/user/head/eyetracking")
//
// Connects as VRApplication_Background (never starts SteamVR, not a Steam app),
// registers a one-action manifest (an "eyetracking" action bound on frame_hmd)
// and prints what IVRInput::GetEyeTrackingDataRelativeToNow returns.
#include <openvr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

int main(int argc, char **argv) {
    const double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    const std::string path = argc > 2 ? argv[2] : "/user/head/eyetracking";

    char dir[] = "/tmp/gaze_probe_XXXXXX";
    if (!mkdtemp(dir)) return 1;
    std::string manifest = std::string(dir) + "/actions.json";
    std::ofstream(std::string(dir) + "/bindings_frame_hmd.json") << R"({
  "controller_type": "frame_hmd",
  "bindings": { "/actions/babble": { "eyetracking": [ { "path": ")" << path << R"(", "output": "/actions/babble/in/gaze" } ] } }
})";
    std::ofstream(manifest) << R"({
  "default_bindings": [ { "controller_type": "frame_hmd", "binding_url": "bindings_frame_hmd.json" } ],
  "actions": [ { "name": "/actions/babble/in/gaze", "type": "eyetracking" } ],
  "action_sets": [ { "name": "/actions/babble", "usage": "single" } ],
  "localization": []
})";

    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    auto *in = vr::VRInput();
    printf("SetActionManifestPath(%s) = %d, binding path %s\n", manifest.c_str(),
           int(in->SetActionManifestPath(manifest.c_str())), path.c_str());
    vr::VRActionSetHandle_t set = 0;
    vr::VRActionHandle_t gaze = 0;
    printf("GetActionSetHandle = %d, GetActionHandle = %d\n", int(in->GetActionSetHandle("/actions/babble", &set)),
           int(in->GetActionHandle("/actions/babble/in/gaze", &gaze)));

    auto t0 = std::chrono::steady_clock::now();
    int n = 0, valid = 0, last_err = -1;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = set;
        in->UpdateActionState(&active, sizeof(active), 1);
        vr::VREyeTrackingData_t d{};
        int e = int(in->GetEyeTrackingDataRelativeToNow(gaze, vr::TrackingUniverseStanding, 0.f, &d, sizeof(d)));
        if (e != last_err) {
            printf("GetEyeTrackingDataRelativeToNow = %d\n", e);
            last_err = e;
        }
        n++;
        if (e == 0 && d.bValid) {
            valid++;
            if (valid % 30 == 1) {
                const auto &o = d.vGazeOrigin.v, &t = d.vGazeTarget.v;
                float dx = t[0] - o[0], dy = t[1] - o[1], dz = t[2] - o[2];
                printf("active=%d valid=%d tracked=%d origin(%.3f %.3f %.3f) target(%.3f %.3f %.3f) dir(%.3f %.3f %.3f)\n",
                       d.bActive, d.bValid, d.bTracked, o[0], o[1], o[2], t[0], t[1], t[2], dx, dy, dz);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
    }
    printf("%d polls, %d valid samples\n", n, valid);
    vr::VR_Shutdown();
    return 0;
}
