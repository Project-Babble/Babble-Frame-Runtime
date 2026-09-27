// Times a model the way the runtime runs it: ExecuTorchRunner, pinned to one
// core, with the process-wide XNNPACK threadpool at the given size.
//
//   model_bench <model.pte> [--core N] [--threads N] [--runs N]
//
// Inputs are a fixed pattern (the model's own shapes and dtypes); prints the
// median, mean and 90th percentile of the run time after a warm-up.
#include "model/executorch_runner.h"
#include "core/clock.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sched.h>

using namespace babble;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: model_bench <model.pte> [--core N] [--threads N] [--runs N]\n");
        return 2;
    }
    int core = -1, threads = 1, runs = 200;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--core") core = atoi(argv[i + 1]);
        else if (a == "--threads") threads = atoi(argv[i + 1]);
        else if (a == "--runs") runs = atoi(argv[i + 1]);
    }
    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        sched_setaffinity(0, sizeof(set), &set);
    }
    ExecuTorchRunner::set_threads(threads);
    ExecuTorchRunner r;
    const uint64_t t_load = mono_ns();
    if (!r.load(argv[1]))
        return 1;
    const double load_ms = double(mono_ns() - t_load) / 1e6;

    auto bytes = [](const TensorSpec &s) { return s.elements() * (s.dtype == DType::F32 ? 4 : s.dtype == DType::F16 ? 2 : 1); };
    std::vector<std::vector<uint8_t>> in, out;
    std::vector<TensorView> iv, ov;
    for (const auto &s : r.inputs()) {
        in.emplace_back(bytes(s));
        for (size_t k = 0; k < in.back().size(); k++)
            in.back()[k] = uint8_t((k * 37 + 13) % 251 / 4);   // small positive values in any dtype
    }
    for (const auto &s : r.outputs())
        out.emplace_back(bytes(s));
    for (size_t i = 0; i < in.size(); i++)
        iv.push_back({in[i].data(), &r.inputs()[i]});
    for (size_t i = 0; i < out.size(); i++)
        ov.push_back({out[i].data(), &r.outputs()[i]});

    for (int i = 0; i < 20; i++)   // warm-up: weight packing, caches, clocks
        if (!r.run(iv, ov))
            return 1;
    std::vector<double> ms;
    for (int i = 0; i < runs; i++) {
        const uint64_t t0 = mono_ns();
        r.run(iv, ov);
        ms.push_back(double(mono_ns() - t0) / 1e6);
    }
    std::sort(ms.begin(), ms.end());
    double mean = 0;
    for (double v : ms)
        mean += v;
    mean /= double(ms.size());
    printf("%s  core %d, %d thread(s): median %.2f ms, mean %.2f, p90 %.2f  (load %.0f ms)\n", argv[1], core, threads,
           ms[ms.size() / 2], mean, ms[ms.size() * 9 / 10], load_ms);
    return 0;
}
