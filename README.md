<p align="center"><img src="runtime/resources/icons/bfr_logo_128.png" alt="Babble Frame Runtime" width="128"></p>

# Babble Frame Runtime

Mouth and eye expression tracking that runs on the Steam Frame itself.

The Steam Frame has two cameras aimed at the mouth and two at the eyes. Babble Frame Runtime turns their images into
face tracking values (jaw, lips, cheeks and tongue, plus eye openness, widening, pupil dilation and per-eye gaze), and
sends them to apps such as VRChat. Everything runs on the headset; no PC is needed.

It is installed as a SteamVR driver. The driver starts the runtime as a separate process and restarts it if it stops,
so a problem in the runtime never affects SteamVR.

## Features

- **Mouth tracking:** 43 blendshapes from the stereo mouth cameras at up to 120 Hz, with a neural network running on
  the CPU (ExecuTorch with XNNPACK).
- **Eye expressions:** openness, widen and pupil dilation per eye, from the eye cameras. Openness is calibrated per
  person automatically, in the style of SRanipal.
- **Gaze:** each eye's own gaze from the headset's eye service, or SteamVR's combined gaze.
- **Presence:** tracking pauses while the headset is off the head (proximity sensor), and the cameras turn off.
- **SteamVR Settings:** an Expression Tracking section to turn mouth and eye tracking on and off, set their rates and
  choose sources.
- **Outputs:**
  - OSC in the Baballonia format for VRCFaceTracking.
  - A plugin interface for other outputs, with a template.
- **A debug overlay:** a SteamVR dashboard tab with live gaze and eye readouts, and a visualizer launcher.

## Status

Work in progress. The mouth and eye models are prototypes; the runtime keeps models swappable so better ones can be
dropped in.

## Quick start

1. Get the source on a PC and copy it to the headset.
2. Build ExecuTorch once, then build the driver, on the headset.
3. Register the driver with `vrpathreg.sh`, then restart SteamVR.

The full steps are in **[docs/building.md](docs/building.md)**.

Once it runs, the runtime sends OSC to `127.0.0.1:8888`, which the Baballonia module for VRCFaceTracking reads. Other
outputs can be added as plugins; see [docs/output-modules.md](docs/output-modules.md).

## Documentation

| Document                                         | Contents                                                       |
|--------------------------------------------------|----------------------------------------------------------------|
| [building.md](docs/building.md)                  | Building, installing, updating, tests, plugins.                |
| [settings.md](docs/settings.md)                  | The Expression Tracking settings and the debug overlay.        |
| [configuration.md](docs/configuration.md)        | Config files and every config key.                             |
| [architecture.md](docs/architecture.md)          | How the runtime is put together.                               |
| [eye-tracking.md](docs/eye-tracking.md)          | Eye models, calibration, gaze and openness sources.            |
| [output-modules.md](docs/output-modules.md)      | The OSC output, the signal list, output plugins and their ABI. |
| [models.md](docs/models.md)                      | The shipped models, exporting and swapping them.               |
| [debugging.md](docs/debugging.md)                | Logs, stats, tools and common problems.                        |

## Repository layout

```
driver/          SteamVR driver shim, settings schema, driver manifest
runtime/
    src/         the runtime: camera, model, state, sink, platform, ui, core
    resources/   default config, models, mouth calibration, fonts, icons
    tests/       unit tests
sdk/             output plugin ABI header and plugin template
tools/           ExecuTorch build script, model export, golden data, probes, icon rendering
third_party/     OpenVR headers, and submodules: ExecuTorch, nlohmann/json, doctest, stb
```
