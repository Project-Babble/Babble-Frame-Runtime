#!/usr/bin/env python3
"""Reference output of a .pte on a fixed synthetic input, for test_executorch_runner.cpp.

Run on the headset with its ExecuTorch Python runtime (same ARM XNNPACK kernels):
    ~/edge-inference/.venv/bin/python tools/golden/make_runner_golden.py <model.pte> <out.json>

Input element k of input i (flattened) is ((k * 37 + i * 91 + 13) % 256) / 255, matching the C++ test.
"""
import json, sys, torch
from executorch.runtime import Runtime

m = Runtime.get().load_program(sys.argv[1]).load_method("forward")
meta = m.metadata
xs = []
for i in range(meta.num_inputs()):
    shape = tuple(meta.input_tensor_meta(i).sizes())
    n = 1
    for d in shape:
        n *= d
    k = torch.arange(n, dtype=torch.int64)
    xs.append((((k * 37 + i * 91 + 13) % 256).float() / 255.0).reshape(shape))
out = m.execute(tuple(xs))
json.dump({"model": sys.argv[1].split("/")[-1], "outputs": [o.flatten().tolist() for o in out]}, open(sys.argv[2], "w"))
print(f"wrote {sys.argv[2]}: {[tuple(o.shape) for o in out]}")
