#!/usr/bin/env python3
"""Export the Baballonia mono face model to an fp16 XNNPACK ExecuTorch .pte, and compare it with ONNX Runtime."""
import argparse, glob, os, sys, time
import numpy as np, onnx, onnxruntime as ort, torch
from onnx2torch import convert
from executorch.backends.xnnpack.partition.xnnpack_partitioner import XnnpackPartitioner
from executorch.exir import to_edge_transform_and_lower
from executorch.runtime import Runtime


def random_inputs(n=8):
    rng = np.random.default_rng(0)
    return [rng.random((1, 1, 224, 224), dtype=np.float32) for _ in range(n)]


def frame_inputs(frames_dir):
    xs = []
    if frames_dir:
        import cv2
        for p in sorted(glob.glob(os.path.join(frames_dir, "*.p[ng]m")) + glob.glob(os.path.join(frames_dir, "*.png"))):
            g = cv2.imread(p, cv2.IMREAD_GRAYSCALE)
            if g is None:
                continue
            g = cv2.resize(g, (224, 224), interpolation=cv2.INTER_LINEAR)
            xs.append((g.astype(np.float32) / 255.0)[None, None])
    return xs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", help="directory of real frames to validate on")
    args = ap.parse_args()

    # Use the basic optimizations of ONNX Runtime to change the shape-based padding into constants for 224x224.
    folded = os.path.splitext(os.path.abspath(args.out))[0] + ".folded.onnx"
    opts = ort.SessionOptions()
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    opts.optimized_model_filepath = folded
    ort.InferenceSession(args.onnx, opts, providers=["CPUExecutionProvider"])
    model = onnx.load(folded)
    os.remove(folded)
    left = sorted({n.op_type for n in model.graph.node} & {"Shape", "ConstantOfShape", "Ceil", "Gather"})
    print("shape ops left after folding:", left or "none")
    # Remove empty optional inputs at the end because onnx2torch reads them as dynamic limits.
    for node in model.graph.node:
        while node.input and node.input[-1] == "":
            node.input.pop()
    module = convert(model).eval()
    example = torch.zeros(1, 1, 224, 224)
    with torch.no_grad():
        ref0 = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"]).run(None, {"x.1": example.numpy()})[0]
        diff = np.abs(module(example).numpy() - ref0).max()
    print(f"onnx2torch vs ONNX Runtime (fp32): max diff {diff:.2e}")

    half = module.half()
    exported = torch.export.export(half, (example.half(),))
    prog = to_edge_transform_and_lower(exported, partitioner=[XnnpackPartitioner()]).to_executorch()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(prog.buffer)
    print(f"wrote {args.out} ({os.path.getsize(args.out) / 1e6:.1f} MB)")

    # The ops that XNNPACK does not take must be in the ops list of tools/build_executorch.sh.
    leftover = sorted({str(n.target) for n in prog.exported_program().graph.nodes
                       if n.op == "call_function" and "executorch_call_delegate" not in str(n.target)
                       and "getitem" not in str(n.target)})
    print("ops outside XNNPACK:", leftover or "none")

    sess = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"])
    pte = Runtime.get().load_program(args.out).load_method("forward")

    def compare(xs):
        worst, times = np.zeros(45), []
        for x in xs:
            ref = sess.run(None, {"x.1": x})[0][0]
            t0 = time.perf_counter()
            out = pte.execute([torch.from_numpy(x).half()])[0].float().numpy()[0]
            times.append(time.perf_counter() - t0)
            worst = np.maximum(worst, np.abs(out - ref))
        return worst, times

    # The random noise result is only for information because only real frames keep fp16 in its precise range.
    worst, _ = compare(random_inputs())
    print(f"fp16 .pte vs ONNX Runtime on random noise (informational): max diff {worst.max():.4f}")
    frames = frame_inputs(args.frames)
    if not frames:
        print("no --frames given: can't validate fp16 on real images")
        return 1
    worst, times = compare(frames)
    print(f"fp16 .pte vs ONNX Runtime on {len(frames)} real frames: max diff {worst.max():.4f} "
          f"(mean over outputs {worst.mean():.4f}); median {1000 * np.median(times):.1f} ms per run")
    return 0 if worst.max() < 0.02 else 1


if __name__ == "__main__":
    sys.exit(main())
