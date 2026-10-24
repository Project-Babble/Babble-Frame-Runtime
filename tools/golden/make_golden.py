#!/usr/bin/env python3
"""Writes OpenCV reference outputs for test_image_ops.cpp, and must run on x86 with opencv-python 5.0.0."""
import sys, pathlib
import cv2, numpy as np

out = pathlib.Path(__file__).resolve().parents[2] / "runtime" / "tests" / "data"
out.mkdir(parents=True, exist_ok=True)

def left_half(path):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    assert img is not None and img.shape == (400, 800), (path, None if img is None else img.shape)
    return np.ascontiguousarray(img[:, :400])

if len(sys.argv) > 1:
    rng = np.random.default_rng(1)
    noise = rng.integers(0, 256, (400, 400), dtype=np.uint8)
    noise[100:300, 50:350] //= 3          # This makes the histogram uneven.
    inputs = {"noise": noise, "mouth": left_half(sys.argv[1]), "eye": left_half(sys.argv[2])}
else:
    inputs = {n: cv2.imread(str(out / f"{n}_in.pgm"), cv2.IMREAD_GRAYSCALE) for n in ("noise", "mouth", "eye")}

for name, img in inputs.items():
    cv2.imwrite(str(out / f"{name}_in.pgm"), img)
    lin = cv2.resize(img, (224, 224), interpolation=cv2.INTER_LINEAR)
    cv2.imwrite(str(out / f"{name}_linear224.pgm"), lin)
    cv2.imwrite(str(out / f"{name}_linear224_eq.pgm"), cv2.equalizeHist(lin))
    cv2.imwrite(str(out / f"{name}_area160.pgm"), cv2.resize(img, (160, 160), interpolation=cv2.INTER_AREA))
    cv2.imwrite(str(out / f"{name}_area96.pgm"), cv2.resize(img, (96, 96), interpolation=cv2.INTER_AREA))
    # Integer INTER_AREA factors take the OpenCV fast path: 4x rounds half to even and 2x (the SIMD path) half up.
    a100 = cv2.resize(img, (100, 100), interpolation=cv2.INTER_AREA)
    cv2.imwrite(str(out / f"{name}_area100.pgm"), a100)
    cv2.imwrite(str(out / f"{name}_arealin96.pgm"), cv2.resize(a100, (96, 96), interpolation=cv2.INTER_LINEAR))
    cv2.imwrite(str(out / f"{name}_area64.pgm"), cv2.resize(img[100:228, 100:228], (64, 64), interpolation=cv2.INTER_AREA))
print(f"opencv {cv2.__version__}: wrote {len(inputs) * 8} files to {out}")
