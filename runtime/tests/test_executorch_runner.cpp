#include <doctest/doctest.h>

#include "model/executorch_runner.h"
#include "model/half.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <vector>

using namespace babble;

#if BABBLE_FRUNK_MOUTH
TEST_CASE("ExecuTorchRunner runs FusionNet and matches the Python ExecuTorch runtime") {
    ExecuTorchRunner::set_threads(1);
    ExecuTorchRunner r;
    REQUIRE(r.load(std::string(BABBLE_MODELS_DIR) + "/fusionnet_int8_xnnpack_qat_linear.pte"));

    // The contract from mouth_osc_pc.py is two float32 [1,1,224,224] inputs and one [1,44] output.
    REQUIRE(r.inputs().size() == 2);
    for (const auto &s : r.inputs()) {
        CHECK(s.dtype == DType::F32);
        CHECK(s.shape == std::vector<int64_t>{1, 1, 224, 224});
    }
    REQUIRE(r.outputs().size() == 1);
    CHECK(r.outputs()[0].shape == std::vector<int64_t>{1, 44});

    // This is the same synthetic input as in tools/golden/make_runner_golden.py.
    std::vector<std::vector<float>> in(2, std::vector<float>(224 * 224));
    for (size_t i = 0; i < 2; i++)
        for (size_t k = 0; k < in[i].size(); k++)
            in[i][k] = float((k * 37 + i * 91 + 13) % 256) / 255.0f;
    std::vector<float> out(44);
    std::vector<TensorView> iv{{in[0].data(), &r.inputs()[0]}, {in[1].data(), &r.inputs()[1]}};
    std::vector<TensorView> ov{{out.data(), &r.outputs()[0]}};
    REQUIRE(r.run(iv, ov));

    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/fusionnet_golden.json");
    auto golden = nlohmann::json::parse(f)["outputs"][0].get<std::vector<float>>();
    REQUIRE(golden.size() == 44);
    float worst = 0;
    for (size_t k = 0; k < 44; k++)
        worst = std::max(worst, std::fabs(out[k] - golden[k]));
    // The int8 output step is about 0.0054, and the test allows one step for XNNPACK version differences.
    CHECK_MESSAGE(worst <= 0.0055f, "max |diff| vs Python ExecuTorch = " << worst);

    // The second run uses the wrapped input tensors again and must give the same result.
    std::vector<float> again(44);
    std::vector<TensorView> ov2{{again.data(), &r.outputs()[0]}};
    REQUIRE(r.run(iv, ov2));
    CHECK(again == out);
}
#endif

namespace {

// Run a .pte on the synthetic input of tools/golden/make_runner_golden.py and compare with its golden file.
void check_runner_golden(const char *model, const char *golden_file, const std::vector<std::vector<int64_t>> &in_shapes,
                         const std::vector<int64_t> &out_shape) {
    ExecuTorchRunner::set_threads(1);
    ExecuTorchRunner r;
    REQUIRE(r.load(std::string(BABBLE_MODELS_DIR) + "/" + model));
    REQUIRE(r.inputs().size() == in_shapes.size());
    std::vector<std::vector<uint16_t>> xh(in_shapes.size());
    std::vector<TensorView> iv;
    for (size_t i = 0; i < in_shapes.size(); i++) {
        CHECK(r.inputs()[i].dtype == DType::F16);
        CHECK(r.inputs()[i].shape == in_shapes[i]);
        const size_t n = r.inputs()[i].elements();
        std::vector<float> x(n);
        for (size_t k = 0; k < n; k++)
            x[k] = float((k * 37 + i * 91 + 13) % 256) / 255.0f;
        xh[i].resize(n);
        to_half(x.data(), xh[i].data(), n);
        iv.push_back({xh[i].data(), &r.inputs()[i]});
    }
    REQUIRE(r.outputs().size() == 1);
    CHECK(r.outputs()[0].dtype == DType::F16);
    CHECK(r.outputs()[0].shape == out_shape);
    const size_t m = r.outputs()[0].elements();
    std::vector<uint16_t> yh(m);
    std::vector<TensorView> ov{{yh.data(), &r.outputs()[0]}};
    REQUIRE(r.run(iv, ov));
    std::vector<float> y(m);
    from_half(yh.data(), y.data(), m);

    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/" + golden_file);
    REQUIRE(f.good());
    auto golden = nlohmann::json::parse(f)["outputs"][0].get<std::vector<float>>();
    REQUIRE(golden.size() == m);
    for (size_t k = 0; k < m; k++)
        CHECK_MESSAGE(std::fabs(y[k] - golden[k]) <= 0.002f, "output " << k << ": " << y[k] << " vs " << golden[k]);
}

}

TEST_CASE("ExecuTorchRunner runs the eye kit stage 1 (lid, pupil centre, Widen) and matches the Python ExecuTorch runtime") {
    check_runner_golden("eye_kit_stage1_fp16.pte", "eye_kit_stage1_golden.json", {{2, 1, 96, 96}}, {2, 4});
}

TEST_CASE("ExecuTorchRunner runs the eye kit stage 2 (pupil mm from a crop) and matches the Python ExecuTorch runtime") {
    check_runner_golden("eye_kit_stage2_fp16.pte", "eye_kit_stage2_golden.json", {{2, 1, 64, 64}, {2, 2}}, {2, 1});
}
