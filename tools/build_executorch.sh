#!/usr/bin/env bash
# Builds and installs the ExecuTorch runtime (XNNPACK backend + the few
# portable kernels our models need) into a local prefix, once. The runtime's
# CMake then finds it with find_package(executorch).
#
#   tools/build_executorch.sh [install-prefix]     (default: <repo>/../et-install)
#
# Build-time only requirement: a Python with torch (for ExecuTorch's kernel
# code generator) in $ET_PYTHON, default the headset venv
# ~/edge-inference/.venv/bin/python.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="${repo}/third_party/executorch"
prefix="${1:-${repo}/../et-install}"
build="${ET_BUILD_DIR:-${repo}/../et-build}"
python="${ET_PYTHON:-${HOME}/edge-inference/.venv/bin/python}"

# Only the nested dependencies an XNNPACK runtime needs (the full set is ~2 GB).
if [[ -d "${src}/.git" || -f "${src}/.git" ]]; then
  git -C "${src}" submodule update --init --depth 1 \
    backends/xnnpack/third-party/{FP16,FXdiv,XNNPACK,cpuinfo,pthreadpool} \
    third-party/flatbuffers third-party/flatcc third-party/json third-party/gflags
fi

# Each XNNPACK delegate gets its own workspace: a shared one is a process-wide
# lock, which serialized the mouth, lid and pupil models across their cores.

# Operators the shipped models leave outside XNNPACK (see tools/export/list_ops.py).
# Keep in sync when adding or re-exporting models.
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
