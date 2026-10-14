#include "platform/curl.h"

#include "core/clock.h"
#include "platform/process.h"

#include <algorithm>

namespace babble::curl {

namespace fs = std::filesystem;

namespace {
std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}
} // namespace

bool fetch(const std::string &url, std::string *out, std::string *err) {
    std::string e;
    const int rc = Process::run({"curl", "-fsSL", "--max-time", "30", "-A", "babble-frame-runtime", url}, out, &e,
                                35000, 8u << 20);
    if (rc != 0) {
        e = trimmed(e);
        *err = e.empty() ? "download failed (curl " + std::to_string(rc) + ")" : e;
        return false;
    }
    return true;
}

bool download(const std::string &url, uint64_t size, const fs::path &to, std::string *err,
              const std::function<bool()> &cancelled, const std::function<void(float)> &progress) {
    Process p;
    Process::Options o;
    o.stderr_pipe = true;
    if (!p.spawn({"curl", "-fsSL", "--retry", "2", "-A", "babble-frame-runtime", "-o", to.string(), url}, o, err))
        return false;
    std::string e;
    while (p.running()) {
        if (cancelled && cancelled()) {
            p.terminate(1000);
            *err = "cancelled";
            return false;
        }
        std::error_code ec;
        const auto have = fs::exists(to, ec) ? fs::file_size(to, ec) : 0;
        if (size > 0 && progress)
            progress(float(std::min<double>(1.0, double(have) / double(size))));
        e += p.read_stderr();
        sleep_ns(100000000ull);
    }
    e += p.read_stderr();
    if (p.exit_code().value_or(-1) != 0) {
        e = trimmed(e);
        *err = e.empty() ? "download failed" : e;
        return false;
    }
    return true;
}

} // namespace babble::curl
