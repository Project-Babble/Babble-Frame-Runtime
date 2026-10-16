#!/usr/bin/env python3
"""Export the lid and pupil eye models to fp32 and int8 ExecuTorch .pte files, and compare int8 with fp32."""
import argparse, glob, math, os, sys, time
import cv2, numpy as np, torch
from torchao.quantization.pt2e.quantize_pt2e import prepare_pt2e, convert_pt2e
from executorch.backends.xnnpack.partition.xnnpack_partitioner import XnnpackPartitioner
from executorch.backends.xnnpack.quantizer.xnnpack_quantizer import XNNPACKQuantizer, get_symmetric_quantization_config
from executorch.exir import to_edge_transform_and_lower
from executorch.runtime import Runtime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from babble_models import EyeNet, load  # noqa: E402

MODELS = {"lid": ("lid_mnv4_160", "mobilenetv4_conv_small", 1280),
          "pupil": ("pupil_cnx_160", "convnext_nano", 640)}


def prep_pair(pane_l, pane_r):
    canon_r = np.ascontiguousarray(np.rot90(pane_r, 2)[:, ::-1])
    x = np.stack([cv2.resize(pane_l, (160, 160), interpolation=cv2.INTER_AREA),
                  cv2.resize(canon_r, (160, 160), interpolation=cv2.INTER_AREA)]).astype(np.float32)
    return ((x / 255.0 - 0.5) / 0.25)[:, None]


def load_frames(d, min_mean=15.0):
    """Loads the eye frames and skips dark frames, because the IR illuminators are off when nobody wears the headset."""
    xs, dark = [], 0
    for p in sorted(glob.glob(os.path.join(d, "eye_*.pgm"))):
        g = cv2.imread(p, cv2.IMREAD_GRAYSCALE)
        if g[:, :400].mean() < min_mean or g[:, 400:800].mean() < min_mean:
            dark += 1
            continue
        # Use standard strides because ExecuTorch export does not accept the stride-0 axis from numpy [:, None].
        xs.append(torch.from_numpy(prep_pair(g[:, :400], g[:, 400:800])).clone(memory_format=torch.contiguous_format))
    if dark:
        print(f"skipped {dark} unlit frames (mean < {min_mean}); was the headset worn?")
    if len(xs) < 100:
        sys.exit(f"only {len(xs)} lit frames in {d}; capture again with the headset worn")
    return xs


def lower(module, example, path):
    prog = to_edge_transform_and_lower(torch.export.export(module, (example,)),
                                       partitioner=[XnnpackPartitioner()]).to_executorch()
    with open(path, "wb") as f:
        f.write(prog.buffer)
    return len(prog.buffer)


def px_of(y):
    return 2.0 * np.sqrt(np.maximum(y, 0.0) * 3000.0 / math.pi)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", choices=list(MODELS), action="append")
    ap.add_argument("--calib-n", type=int, default=200)
    ap.add_argument("--val-n", type=int, default=200)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    frames = load_frames(a.frames)
    calib, val = frames[0::2][: a.calib_n], frames[1::2][: a.val_n]
    print(f"{len(frames)} frames: {len(calib)} calibration, {len(val)} validation")

    for key in a.only or list(MODELS):
        name, arch, feat = MODELS[key]
        _, fp = load(os.path.join(a.models, name + ".pt"), EyeNet(arch, feat))
        ex = calib[0]
        n32 = lower(fp, ex, os.path.join(a.out, f"{name}_fp32.pte"))

        g = torch.export.export(fp, (ex,)).module()
        g = prepare_pt2e(g, XNNPACKQuantizer().set_global(get_symmetric_quantization_config(is_per_channel=True)))
        t = time.time()
        with torch.no_grad():
            for x in calib:
                g(x)
        g = convert_pt2e(g)
        out8 = os.path.join(a.out, f"{name}_int8.pte")
        n8 = lower(g, ex, out8)
        print(f"{name}: fp32 {n32/1e6:.1f} MB, int8 {n8/1e6:.1f} MB (calibrated in {time.time()-t:.0f} s)")

        m8 = Runtime.get().load_program(out8).load_method("forward")
        ref, q = [], []
        with torch.no_grad():
            for x in val:
                ref.append(fp(x).numpy())
                q.append(m8.execute((x,))[0].numpy())
        ref, q = np.concatenate(ref), np.concatenate(q)
        d = np.abs(ref - q)
        print(f"  {key} fp32 range {ref.min():.3f}..{ref.max():.3f}; int8 |diff| mean {d.mean():.4f} "
              f"p99 {np.percentile(d, 99):.4f} max {d.max():.4f}")
        if key == "pupil":
            dpx = np.abs(px_of(ref) - px_of(q))
            print(f"  pupil diameter |diff| px: mean {dpx.mean():.2f} p99 {np.percentile(dpx, 99):.2f} "
                  f"max {dpx.max():.2f} (fp32 diameter {px_of(ref).mean():.1f} px)")
        if key == "lid":
            # Measure how frequently the int8 model changes the pupil gate decision of the fp32 model.
            flips = np.mean((ref < 0.4) != (q < 0.4)) * 100
            print(f"  lid gate (0.4) disagreement: {flips:.2f}% of eye samples")


if __name__ == "__main__":
    main()
