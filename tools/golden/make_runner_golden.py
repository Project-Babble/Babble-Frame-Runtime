#!/usr/bin/env python3
"""Writes the reference output of a .pte on a fixed synthetic input for test_executorch_runner.cpp, on the headset."""
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
    x = (((k * 37 + i * 91 + 13) % 256).float() / 255.0).reshape(shape)
    half = int(meta.input_tensor_meta(i).dtype()) == 5   # The value 5 is ScalarType::Half.
    xs.append(x.half() if half else x)
out = m.execute(tuple(xs))
json.dump({"model": sys.argv[1].split("/")[-1], "outputs": [o.float().flatten().tolist() for o in out]},
          open(sys.argv[2], "w"))
print(f"wrote {sys.argv[2]}: {[tuple(o.shape) for o in out]}")
