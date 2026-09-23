#!/usr/bin/env python3
"""Writes OpenCV reference outputs for runtime/tests/test_image_ops.cpp.

Must run with the same OpenCV the reference scripts use (opencv-python 5.0.0,
e.g. ~/etenv/bin/python), since the runtime has to match it bit for bit:

    ~/etenv/bin/python tools/golden/make_golden.py <mouth_dump.pgm> <eye_dump.pgm>

The dump files are DebugSink side-by-side stereo PGMs (800x400); the left
400x400 half of each is used.
"""
import sys, pathlib
import cv2, numpy as np

out = pathlib.Path(__file__).resolve().parents[2] / "runtime" / "tests" / "data"
out.mkdir(parents=True, exist_ok=True)

def left_half(path):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    assert img is not None and img.shape == (400, 800), (path, None if img is None else img.shape)
    return np.ascontiguousarray(img[:, :400])

rng = np.random.default_rng(1)
noise = rng.integers(0, 256, (400, 400), dtype=np.uint8)
noise[100:300, 50:350] //= 3          # uneven histogram
inputs = {"noise": noise, "mouth": left_half(sys.argv[1]), "eye": left_half(sys.argv[2])}

for name, img in inputs.items():
    cv2.imwrite(str(out / f"{name}_in.pgm"), img)
    lin = cv2.resize(img, (224, 224), interpolation=cv2.INTER_LINEAR)
    cv2.imwrite(str(out / f"{name}_linear224.pgm"), lin)
    cv2.imwrite(str(out / f"{name}_linear224_eq.pgm"), cv2.equalizeHist(lin))
    cv2.imwrite(str(out / f"{name}_area160.pgm"), cv2.resize(img, (160, 160), interpolation=cv2.INTER_AREA))
print(f"opencv {cv2.__version__}: wrote {len(inputs) * 4} files to {out}")
