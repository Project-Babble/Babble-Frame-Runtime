#pragma once

#include "model/model_runner.h"
#include "model/pipeline.h"

namespace babble {

// Config type "null": preprocesses to a side-by-side gray input and runs
// NullModelRunner (all zeros). For tests and for bringing up new sources
// without a model.
class NullPipeline : public PipelineBase {
public:
    NullPipeline(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "inf") {}
    bool init() override;

private:
    void process(const StereoFrame &frames) override;

    std::unique_ptr<IModelRunner> runner_;
    uint32_t in_h_ = 0, in_w_ = 0;
    std::vector<std::vector<uint8_t>> in_buffers_, out_buffers_;
    std::vector<std::vector<SignalId>> out_signals_;
};

} // namespace babble
