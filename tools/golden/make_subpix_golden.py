#!/usr/bin/env python3
"""Writes cv2.getRectSubPix references for test_image_ops.cpp. It must run on the headset (aarch64 opencv-python
5.0.0), whose OpenCV uses the portable code: the x86 wheels use IPP for this call, which rounds exact ties down."""
import json, pathlib, platform
import cv2, numpy as np

assert platform.machine() == "aarch64", "run this on the headset: the x86 OpenCV uses IPP for getRectSubPix"
out = pathlib.Path(__file__).resolve().parents[2] / "runtime" / "tests" / "data"
rng = np.random.default_rng(7)
# The centres cover the inside path, half-pixel ties, each edge, the corners and windows fully outside the image.
centres = [(200.3, 170.7), (64.0, 64.0), (63.5, 63.5), (335.49, 335.51), (336.2, 200.0), (10.5, 390.25), (-5.5, 205.1),
           (405.7, -3.2), (-200.0, 600.0)] + [tuple(float(c) for c in v) for v in rng.uniform(-30, 430, (7, 2))]
centres = [(float(np.float32(x)), float(np.float32(y))) for x, y in centres]
for name in ("noise", "eye"):
    img = cv2.imread(str(out / f"{name}_in.pgm"), cv2.IMREAD_GRAYSCALE)
    for k, c in enumerate(centres):
        cv2.imwrite(str(out / f"{name}_sub{k}.pgm"), cv2.getRectSubPix(img, (128, 128), c))
json.dump({"size": 128, "centres": centres}, open(out / "subpix_golden.json", "w"))
print(f"opencv {cv2.__version__} ({platform.machine()}): wrote {2 * len(centres)} crops to {out}")
