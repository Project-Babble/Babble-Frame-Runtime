#include <doctest/doctest.h>

#include "model/executorch_runner.h"
#include "model/half.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <vector>

using namespace babble;

TEST_CASE("ExecuTorchRunner runs FusionNet and matches the Python ExecuTorch runtime") {
    ExecuTorchRunner::set_threads(1);
    ExecuTorchRunner r;
    REQUIRE(r.load(std::string(BABBLE_MODELS_DIR) + "/fusionnet_int8_xnnpack_qat_linear.pte"));

    // Contract from mouth_osc_pc.py: two float32 [1,1,224,224] inputs, one [1,44] output.
    REQUIRE(r.inputs().size() == 2);
    for (const auto &s : r.inputs()) {
        CHECK(s.dtype == DType::F32);
        CHECK(s.shape == std::vector<int64_t>{1, 1, 224, 224});
    }
    REQUIRE(r.outputs().size() == 1);
    CHECK(r.outputs()[0].shape == std::vector<int64_t>{1, 44});

    // Same synthetic input as tools/golden/make_runner_golden.py.
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
    // int8 output step is ~0.0054; allow one step for XNNPACK version differences.
    CHECK_MESSAGE(worst <= 0.0055f, "max |diff| vs Python ExecuTorch = " << worst);

    // Second run reuses the wrapped input tensors and must give the same result.
    std::vector<float> again(44);
    std::vector<TensorView> ov2{{again.data(), &r.outputs()[0]}};
    REQUIRE(r.run(iv, ov2));
    CHECK(again == out);
}

TEST_CASE("ExecuTorchRunner runs the fp16 combined eye model and matches the Python ExecuTorch runtime") {
    ExecuTorchRunner::set_threads(1);
    ExecuTorchRunner r;
    REQUIRE(r.load(std::string(BABBLE_MODELS_DIR) + "/eye_split96_uncapped_fp16.pte"));

    // Contract from its README: fp16 [2,1,96,96] in, fp16 [2,2] (lid, pupil per eye) out; lid uncapped.
    REQUIRE(r.inputs().size() == 1);
    CHECK(r.inputs()[0].dtype == DType::F16);
    CHECK(r.inputs()[0].shape == std::vector<int64_t>{2, 1, 96, 96});
    REQUIRE(r.outputs().size() == 1);
    CHECK(r.outputs()[0].dtype == DType::F16);
    CHECK(r.outputs()[0].shape == std::vector<int64_t>{2, 2});

    const size_t n = 2 * 96 * 96;
    std::vector<float> x(n);
    for (size_t k = 0; k < n; k++)
        x[k] = float((k * 37 + 13) % 256) / 255.0f;
    std::vector<uint16_t> xh(n), yh(4);
    to_half(x.data(), xh.data(), n);
    std::vector<TensorView> iv{{xh.data(), &r.inputs()[0]}};
    std::vector<TensorView> ov{{yh.data(), &r.outputs()[0]}};
    REQUIRE(r.run(iv, ov));
    std::vector<float> y(4);
    from_half(yh.data(), y.data(), 4);

    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/eye_split96_golden.json");
    auto golden = nlohmann::json::parse(f)["outputs"][0].get<std::vector<float>>();
    REQUIRE(golden.size() == 4);
    for (size_t k = 0; k < 4; k++)
        CHECK_MESSAGE(std::fabs(y[k] - golden[k]) <= 0.002f, "output " << k << ": " << y[k] << " vs " << golden[k]);
}
