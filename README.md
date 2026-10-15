<p align="center"><img src="runtime/resources/icons/bfr_logo_128.png" alt="Babble Frame Runtime" width="128"></p>

# Babble Frame Runtime

Eye and mouth expression tracking that runs on the Steam Frame itself.

Babble Frame Runtime reads the Steam Frame's eye cameras, and a wired Babble face tracker for the mouth, and runs a set of optimized neural networks to output expression values (eye openness, widening, pupil dilation and per-eye gaze; jaw, lips, cheeks and
tongue), which it sends to VRCFaceTracking on your PC.

It is installed as a SteamVR driver for settings integration. The driver starts the runtime as a separate process and restarts it if it stops,
so a problem in the runtime never affects SteamVR.

## Features

- **Eye expressions:** openness, widen and pupil dilation per eye.
- **Mouth tracking:** 43 blendshapes from a wired Babble face tracker plugged into the headset, with a neural network running on the CPU (ExecuTorch with XNNPACK).
- **Gaze:** each eye's own gaze from the headset's eye service, or SteamVR's combined gaze.
- **Presence:** tracking pauses while the headset is off the head (proximity sensor), and the cameras turn off.
- **SteamVR Settings:** an Expression Tracking section to turn mouth and eye tracking on and off and set the eye
  tracking rate.
- **Outputs:**
  - OSC in the Baballonia format for VRCFaceTracking.
  - A plugin interface for other outputs, with a template.
- **An app window:** a Babble tab in the SteamVR dashboard with live tracking status, settings and diagnostics. Turn it
  on with Show App Window in SteamVR Settings, under Expression Tracking.
- **Dataset capture:** an optional guided recording app, installed from the overlay's Diagnostics page. The runtime
  records every camera frame for it (lossless x264 through ffmpeg), then packages (hardware H.265), encrypts and
  uploads the sessions the user consented to.

## Status

Work in progress. The mouth and eye models are prototypes; the runtime keeps models swappable so better ones can be
dropped in.

## Install

On the Steam Frame, switch to Desktop Mode, open Konsole and paste:

```
curl -fsSL https://raw.githubusercontent.com/Project-Babble/Babble-Frame-Runtime/main/tools/get.sh | bash
```

It downloads the newest release, checks it against the release's SHA-256 checksum, installs it in your home folder
(no sudo) and restarts SteamVR. Updates after that appear in the SteamVR dashboard's Babble tab, next to the version.
To uninstall, run `~/.local/share/babble-frame-runtime/uninstall.sh`.

## Building from source

1. Get the source on a PC and copy it to the headset.
2. Build ExecuTorch once, then build the driver, on the headset.
3. Register the driver with `vrpathreg.sh`, then restart SteamVR.


Once it runs, the runtime sends OSC to `127.0.0.1:8888`, which the Baballonia module for VRCFaceTracking reads. Other
outputs can be added as plugins.

## Build profiles

`-DBABBLE_PROFILE=release` (what releases ship) builds the USB Babble tracker as the only mouth and VRCFaceTracking as
the only output; it leaves out the Frame's front-port mouth cameras and their camera arbiter, the VRChat-native output,
the visualizer launcher, the camera-stream switch and the unused models. The default, `dev`, builds everything. The
profile's `defaults.json` and SteamVR settings files are derived at configure time from the full ones
(`cmake/babble_profile.cmake`), so there is one source of truth.

## License

Licensed under the [Babble Software Distribution License 1.0](LICENSE) non-commercial use only, and derivative works must be released under the same license with their source. Third-party code in `third_party/` keeps its own license.
