#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame.h"
#include "frame/mailbox.h"
#include "model/model_runner.h"
#include "state/expression_state.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class DebugSink;

// Config-driven inference stage: takes the latest stereo frame from its
// source, preprocesses it into the runner's input tensors, runs the model and
// writes the output signals into ExpressionState.
//
// A pipeline's "inputs" name a source ("mouth" or "eye") and a layout; its
// "outputs" map each output tensor's values, in order, to signal names. A
// merged model later is one pipeline with inputs from both sources.
class Pipeline {
public:
    Pipeline(std::string name, const nlohmann::json &cfg, ThreadSpec thread, LatestSlot<StereoFrame> &input,
             ExpressionState &state, StatsRegistry &stats, DebugSink *debug);
    ~Pipeline();

    bool init();   // builds the runner and validates tensor specs against the config
    void start();
    void stop();

private:
    void run();

    std::string name_;
    nlohmann::json cfg_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &input_;
    ExpressionState &state_;
    StatsRegistry &stats_;
    DebugSink *debug_;

    std::unique_ptr<IModelRunner> runner_;
    uint32_t in_h_ = 0, in_w_ = 0;
    std::vector<std::vector<uint8_t>> in_buffers_, out_buffers_;
    std::vector<std::vector<SignalId>> out_signals_;   // per output tensor

    std::thread thread_;
    std::atomic<bool> stop_{false};

    RateStat rate_;
    LatencyStat preprocess_, inference_, capture_to_state_;
};

} // namespace babble
