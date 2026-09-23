#!/usr/bin/env python3
"""Reference sequences for test_postprocess.cpp, produced by the PC scripts' own classes.

    <python with numpy> tools/golden/make_post_golden.py <dir with mouth_osc_pc.py/eye_osc_pc.py/calibration_mouth.json>

Mouth: the main-loop chain of mouth_osc_pc.py (lines 540-552) in manual and auto modes.
Eye:   the per-update chain of eye_osc_pc.py (lines 511-517) with a pupil result every 18th update (5 Hz at 90).
"""
import json, math, pathlib, sys
import numpy as np

ref = pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0, str(ref))
import mouth_osc_pc as M   # noqa: E402
import eye_osc_pc as E     # noqa: E402

rng = np.random.default_rng(7)
out = {}

# --- mouth: presence goes absent -> present -> absent (long enough for a reset) -> present
N = 260
preds = rng.random((N, M.NUM_CHANNELS)).astype(np.float32) * 0.8
presence = np.concatenate([np.full(10, 0.1), np.full(90, 0.95), np.full(70, 0.05), np.full(90, 0.9)]).astype(np.float32)
preds[:, M.PRESENCE_IDX] = presence
cal = M.Calibration.load(str(ref / "calibration_mouth.json"))
for mode in ("manual", "auto"):
    gate, auto = M.PresenceGate(0.5), M.AutoCalibrator()
    raw_state, rows = None, []
    for pred in preds:
        raw_state = pred if raw_state is None else 0.5 * pred + (1 - 0.5) * raw_state
        present = float(np.clip(raw_state[M.PRESENCE_IDX], 0.0, 1.0))
        active, should_reset = gate.update(present)
        if should_reset: auto.reset()
        norm = cal.normalize(raw_state) if mode == "manual" else auto.process(pred, active)
        norm = np.array(norm, np.float32)
        norm[M.PRESENCE_IDX] = present
        if not active: norm[:M.PRESENCE_IDX] = 0.0
        rows.append(norm.tolist())
    out[f"mouth_{mode}"] = {"expected": rows}
out["mouth_preds"] = preds.tolist()

# --- eye: lids open, a blink, a squint, wide; pupil sizes drifting
hz, pupil_every = 90, 18
T = 900
t = np.arange(T)
lid = 0.92 + 0.03 * np.sin(t / 37.0)
lid[300:312] = np.linspace(0.9, 0.05, 12)          # blink
lid[312:330] = 0.05
lid[330:342] = np.linspace(0.05, 0.9, 12)
lid[600:660] = 0.45                                  # squint around the gate
lid[700:760] = 1.08                                  # wide
lid_r = np.clip(lid - 0.06 + 0.01 * np.cos(t / 23.0), 0, 1.2)
lids = np.stack([np.clip(lid, 0, 1), np.clip(lid_r, 0, 1)], 1)
pupil = np.stack([0.7 + 0.1 * np.sin(t / 150.0), 0.75 + 0.08 * np.cos(t / 170.0)], 1)
calib = [E.LidCalibrator(int(20 * hz), int(3 * hz)) for _ in range(2)]
ptrk = [E.PupilTracker(0.4, 0.1, int(0.1 * hz), 0.2, 0.6, max(10, int(60 * 5)), max(5, int(5))) for _ in range(2)]
sm, rows = [None, None], []
for i in range(T):
    py = pupil[i] if i % pupil_every == 0 else None
    o = [0.0, 0.0]
    for e in range(2):
        v = calib[e].update(float(lids[i, e]))
        sm[e] = v if sm[e] is None else sm[e] + 0.6 * (v - sm[e])
        o[e] = max(sm[e], 0.0)
    row = {"o": o, "ref": [calib[0].ref, calib[1].ref], "px": [], "ratio": [], "live": [], "dil": []}
    for e in range(2):
        px, ratio, live = ptrk[e].update(o[e], None if py is None else float(py[e]))
        dil = None if ratio is None else max(0.0, min(1.0, 0.5 + 2.0 * (ratio - 1.0)))
        row["px"].append(px); row["ratio"].append(ratio); row["live"].append(bool(live)); row["dil"].append(dil)
    rows.append(row)
out["eye"] = {"lids": lids.tolist(), "pupil": pupil.tolist(), "pupil_every": pupil_every, "hz": hz, "expected": rows}

dst = pathlib.Path(__file__).resolve().parents[2] / "runtime" / "tests" / "data" / "post_golden.json"
dst.write_text(json.dumps(out))
print(f"wrote {dst} ({dst.stat().st_size // 1024} KB)")
