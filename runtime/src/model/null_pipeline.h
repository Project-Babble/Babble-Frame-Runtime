#pragma once

#include "model/model_runner.h"
#include "model/pipeline.h"

namespace babble {

// The "null" pipeline runs NullModelRunner on a side-by-side gray input, for tests and new sources.
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

}
