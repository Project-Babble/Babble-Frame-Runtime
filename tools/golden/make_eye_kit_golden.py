#!/usr/bin/env python3
"""Writes the eye kit end-to-end reference for test_eye_kit.cpp from the reference set packed with the models
(ref.npz from SAM3-Frame EyeExpr/code/pack_kit.py: canonical 400 px frame pairs and the fp32 PyTorch outputs of the
whole chain). The test runs the fp16 .pte files in C++, so it compares with tolerances.
    make_eye_kit_golden.py <ref.npz> [pairs ...]"""
import json, pathlib, sys
import cv2, numpy as np

out = pathlib.Path(__file__).resolve().parents[2] / "runtime" / "tests" / "data"
ref = np.load(sys.argv[1], allow_pickle=True)
pick = [int(p) for p in sys.argv[2:]] or [20, 100, 180]
cases = []
for k, i in enumerate(pick):
    for v in range(2):
        cv2.imwrite(str(out / f"eyekit_pair{k}_v{v}.pgm"), ref["frames"][i][v])
    s1, s2 = ref["stage1"][i], ref["stage2"][i]
    cases.append({"session": str(ref["session"][i]), "frame": int(ref["frame"][i]), "lid": [float(s1[v, 0]) for v in range(2)],
                  "x": [float(400 * s1[v, 1] - 0.5) for v in range(2)], "y": [float(400 * s1[v, 2] - 0.5) for v in range(2)],
                  "mm": [float(2 + 5 * s2[v]) for v in range(2)]})
    if s1.shape[1] > 3:   # A stage 1 with a Widen head gives its raw output as the fourth value.
        cases[-1]["widen"] = [float(s1[v, 3]) for v in range(2)]
json.dump({"source": pathlib.Path(sys.argv[1]).name, "pairs": cases}, open(out / "eye_kit_golden.json", "w"), indent=1)
print(f"wrote {len(cases)} pairs to {out}")
