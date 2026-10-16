#!/usr/bin/env bash
# Build and install the ExecuTorch runtime one time, with $ET_PYTHON set to a Python that has torch.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="${repo}/third_party/executorch"
prefix="${1:-${repo}/../et-install}"
build="${ET_BUILD_DIR:-${repo}/../et-build}"
python="${ET_PYTHON:-${HOME}/edge-inference/.venv/bin/python}"

# Get only the nested dependencies that XNNPACK needs, because the full set is about 2 GB.
if [[ -d "${src}/.git" || -f "${src}/.git" ]]; then
  git -C "${src}" submodule update --init --depth 1 \
    backends/xnnpack/third-party/{FP16,FXdiv,XNNPACK,cpuinfo,pthreadpool} \
    third-party/flatbuffers third-party/flatcc third-party/json third-party/gflags
fi

# Each XNNPACK delegate has its own workspace because a shared workspace is one lock for all the models.

# Update this list of the operators that are not in XNNPACK when you add or export a model again.
ops="aten::alias_copy.out,aten::as_strided_copy.out,aten::index.Tensor_out,aten::native_layer_norm.out"
ops+=",aten::split_with_sizes_copy.out,aten::squeeze_copy.dims_out,aten::view_copy.out,aten::permute_copy.out"
ops+=",aten::clone.out,aten::cat.out,aten::clamp.out,aten::mean.out,aten::add.out,aten::mul.out"

cmake -S "${src}" -B "${build}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${prefix}" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DPYTHON_EXECUTABLE="${python}" \
  -DEXECUTORCH_BUILD_XNNPACK=ON \
  -DEXECUTORCH_XNNPACK_SHARED_WORKSPACE=OFF \
  -DEXECUTORCH_BUILD_EXTENSION_MODULE=ON \
  -DEXECUTORCH_BUILD_EXTENSION_DATA_LOADER=ON \
  -DEXECUTORCH_BUILD_EXTENSION_FLAT_TENSOR=ON \
  -DEXECUTORCH_BUILD_EXTENSION_NAMED_DATA_MAP=ON \
  -DEXECUTORCH_BUILD_EXTENSION_TENSOR=ON \
  -DEXECUTORCH_BUILD_PORTABLE_OPS=ON \
  -DEXECUTORCH_SELECT_OPS_LIST="${ops}" \
  -DEXECUTORCH_ENABLE_LOGGING=ON \
  -DEXECUTORCH_BUILD_EXECUTOR_RUNNER=OFF \
  -DEXECUTORCH_BUILD_DEVTOOLS=OFF \
  -DEXECUTORCH_BUILD_PYBIND=OFF
cmake --build "${build}" -j"$(nproc)"
cmake --install "${build}"
echo "ExecuTorch installed to ${prefix}"
