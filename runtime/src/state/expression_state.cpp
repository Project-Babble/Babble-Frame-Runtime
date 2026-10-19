#include "state/expression_state.h"

namespace babble {

SignalId ExpressionState::add_signal(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (SignalId i = 0; i < names_.size(); i++)
        if (names_[i] == name)
            return i;
    names_.push_back(name);
    signals_.emplace_back();
    return SignalId(names_.size() - 1);
}

void ExpressionState::update(std::span<const std::pair<SignalId, Signal>> values) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[id, s] : values)
            if (id < signals_.size())
                signals_[id] = s;
        seq_++;
    }
    cv_.notify_all();
}

uint64_t ExpressionState::read(std::span<const SignalId> ids, Signal *out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < ids.size(); i++)
        out[i] = ids[i] < signals_.size() ? signals_[ids[i]] : Signal{};
    return seq_;
}

size_t ExpressionState::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return names_.size();
}

std::string ExpressionState::name(SignalId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id < names_.size() ? names_[id] : std::string();
}

ExpressionState::Snapshot ExpressionState::snapshot() const {
    Snapshot out;
    snapshot(out);
    return out;
}

void ExpressionState::snapshot(Snapshot &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out.seq = seq_;
    out.names = names_;
    out.signals = signals_;
}

uint64_t ExpressionState::wait_update(uint64_t after_seq, std::chrono::nanoseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return seq_ > after_seq; });
    return seq_;
}

}
