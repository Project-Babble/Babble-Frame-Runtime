#include "capture/uploader.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sha256.h"
#include "core/threading.h"
#include "platform/process.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("upload");
} // namespace

namespace babble::capture {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        return json();
    json j = json::parse(f, nullptr, false);
    return j.is_discarded() ? json() : j;
}

bool write_json(const fs::path &p, const json &j) {
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp);
        f << j.dump(2) << '\n';
        if (!f)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

bool valid_session_id(const std::string &s) {
    return !s.empty() && s.size() < 64 && s.find('/') == std::string::npos && s.find("..") == std::string::npos &&
           s[0] != '.';
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

} // namespace

Uploader::Uploader(const json &cfg, fs::path captures_dir, const json &threads, std::function<bool()> paused,
                   Transport transport, Packager packager)
    : endpoint_(cfg.value("endpoint", "")), bucket_(cfg.value("bucket", "")), region_(cfg.value("region", "garage")),
      access_key_(cfg.value("access_key", "")), secret_key_(cfg.value("secret_key", "")),
      public_key_(cfg.value("public_key", "")), dir_(std::move(captures_dir)),
      thread_spec_(thread_spec(threads, "net", "bbl-net")),
      // Packaging runs the encoder pipeline: on the recorder's cpus, which are
      // free because nothing is packaged while recording.
      package_spec_(thread_spec(threads, "record", "bbl-package")), paused_(std::move(paused)),
      transport_(transport ? std::move(transport) : Transport(curl_transport)),
      packager_(packager ? std::move(packager) : Packager(make_package)) {
    while (!endpoint_.empty() && endpoint_.back() == '/')
        endpoint_.pop_back();
    package_options_.qp = cfg.value("qp", package_options_.qp);
}

Uploader::~Uploader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool Uploader::available() const {
    return !endpoint_.empty() && !bucket_.empty() && !access_key_.empty() && !secret_key_.empty() &&
           public_key_.find("BEGIN PUBLIC KEY") != std::string::npos;
}

void Uploader::start() {
    if (!available() || thread_.joinable())
        return;
    count_pending();
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

Uploader::Snapshot Uploader::snapshot() const {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    return snap_;
}

void Uploader::set_stage(const std::string &stage, float progress) {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    snap_.stage = stage;
    snap_.progress = progress;
}

Uploader::Outcome Uploader::classify(int status) {
    if (status >= 200 && status < 300)
        return Outcome::Ok;
    if (status >= 400 && status < 500 && status != 408 && status != 429)
        return Outcome::Fatal;
    return Outcome::Retry;   // 0 (no response), 408, 429, 5xx
}

std::string Uploader::object_name(const std::string &contributor, const std::string &routine,
                                  const std::string &session, time_t when) {
    char ts[32];
    tm local{};
    localtime_r(&when, &local);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &local);
    std::string r;
    for (char c : routine)
        r += isalnum(static_cast<unsigned char>(c)) || c == '-' ? char(tolower(static_cast<unsigned char>(c))) : '_';
    if (r.empty())
        r = "session";
    // Ingest collects .bin objects; "frame-...-v2_" is what consumers branch on.
    return contributor + "_" + ts + "_frame-" + r + "-v2_" + session + ".tar.bin";
}

bool Uploader::make_envelope(const fs::path &in, const fs::path &out, const fs::path &pem, std::string *err,
                             const std::string &openssl) {
    auto fail = [&](const std::string &why) {
        if (err)
            *err = why;
        std::error_code ec;
        fs::remove(out.string() + ".part", ec);
        return false;
    };
    // A fresh AES-256 key and IV.
    uint8_t secret[48];
    {
        const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        const bool ok = fd >= 0 && ::read(fd, secret, sizeof(secret)) == ssize_t(sizeof(secret));
        if (fd >= 0)
            ::close(fd);
        if (!ok)
            return fail("no random numbers");
    }
    auto hex = [](const uint8_t *p, size_t n) {
        static const char *k = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < n; i++)
            s += {k[p[i] >> 4], k[p[i] & 15]};
        return s;
    };
    // Wrap the key: RSA-OAEP with SHA-256 for both the hash and MGF1, as .NET's
    // RSAEncryptionPadding.OaepSHA256 (what Baballonia's ingest expects).
    Process wrap;
    Process::Options wo;
    wo.stdin_pipe = true;
    wo.stdout_pipe = true;
    wo.stderr_pipe = true;
    std::string perr;
    if (!wrap.spawn({openssl, "pkeyutl", "-encrypt", "-pubin", "-inkey", pem.string(), "-pkeyopt",
                     "rsa_padding_mode:oaep", "-pkeyopt", "rsa_oaep_md:sha256", "-pkeyopt", "rsa_mgf1_md:sha256"},
                    wo, &perr))
        return fail(openssl + ": " + perr);
    const bool sent = wrap.write_stdin(secret, 32, 5000);
    wrap.close_stdin();
    std::string wrapped;
    for (int i = 0; i < 300 && wrap.running(); i++) {
        wrapped += wrap.read_stdout();
        sleep_ns(10000000);
    }
    wrapped += wrap.read_stdout();
    if (!sent || !wrap.wait(1000) || wrap.exit_code().value_or(-1) != 0 || wrapped.empty())
        return fail("wrapping the key failed: " + trim(wrap.read_stderr()));

    const fs::path part = out.string() + ".part";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        const uint32_t n = uint32_t(wrapped.size());
        const uint8_t len[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
        f.write(reinterpret_cast<const char *>(len), 4);
        f.write(wrapped.data(), std::streamsize(wrapped.size()));
        f.write(reinterpret_cast<const char *>(secret + 32), 16);
        if (!f)
            return fail("cannot write " + part.string());
    }
    // The body, appended by openssl (PKCS#7 padding, as .NET's default). The
    // key is on its command line for the few seconds this takes; it only
    // protects this one file, which is readable on this machine anyway.
    Process enc;
    Process::Options eo;
    eo.stdout_file = part.string();
    eo.stderr_pipe = true;
    if (!enc.spawn({openssl, "enc", "-aes-256-cbc", "-K", hex(secret, 32), "-iv", hex(secret + 32, 16), "-in",
                    in.string()},
                   eo, &perr))
        return fail(openssl + ": " + perr);
    memset(secret, 0, sizeof(secret));
    if (!enc.wait(600000) || enc.exit_code().value_or(-1) != 0)
        return fail("encrypting failed: " + trim(enc.read_stderr()));
    std::error_code ec;
    const uint64_t in_bytes = fs::file_size(in, ec), want = 4 + wrapped.size() + 16 + (in_bytes / 16 + 1) * 16;
    if (ec || fs::file_size(part, ec) != want)
        return fail("the encrypted file has the wrong size");
    fs::rename(part, out, ec);
    if (ec)
        return fail("cannot rename " + part.string() + ": " + ec.message());
    return true;
}

Result Uploader::queue(const std::string &session) {
    if (!available())
        return Result::fail("unavailable", "no upload destination is configured");
    if (!valid_session_id(session))
        return Result::fail("bad_request", "not a session id");
    const fs::path sdir = dir_ / session;
    const json s = read_json(sdir / "session.json");
    if (!s.is_object())
        return Result::fail("bad_request", "no such session");
    if (s.value("status", "") != "complete")
        return Result::fail("bad_request", "the session isn't complete");
    if (!s.contains("consent") || s["consent"].is_null())
        return Result::fail("bad_request", "the session has no consent; it can't be uploaded");
    json u = read_json(sdir / "upload.json");
    if (u.is_object() && u.value("state", "") == "done")
        return Result::fail("bad_request", "already uploaded");
    if (!u.is_object())
        u = json::object();
    u["state"] = "queued";
    u.erase("error");
    if (!write_json(sdir / "upload.json", u))
        return Result::fail("io", "cannot write upload.json");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retry_at_.erase(session);
        backoff_.erase(session);
        kick_ = true;
    }
    cv_.notify_one();
    count_pending();
    LOG.info("upload: session %s queued", session.c_str());
    return {};
}

void Uploader::count_pending() {
    int n = 0;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir_, ec)) {
        const json u = read_json(e.path() / "upload.json");
        if (u.is_object() && (u.value("state", "") == "queued" || u.value("state", "") == "uploading"))
            n++;
    }
    std::lock_guard<std::mutex> lock(snap_mutex_);
    snap_.pending = n;
}

std::string Uploader::contributor_id() {
    const fs::path p = paths::state_dir() / "contributor_id";
    std::string id;
    if (std::ifstream f(p); f)
        std::getline(f, id);
    id = trim(id);
    if (id.size() == 32)
        return id;
    std::random_device rd;
    static const char *kHex = "0123456789abcdef";
    id.clear();
    for (int i = 0; i < 32; i++)
        id += kHex[rd() & 15];
    std::ofstream(p) << id << '\n';
    LOG.info("upload: new contributor id %s...", id.substr(0, 4).c_str());
    return id;
}

bool Uploader::upload_one(const fs::path &sdir) {
    const std::string session = sdir.filename().string();
    json u = read_json(sdir / "upload.json");
    const json s = read_json(sdir / "session.json");
    auto fatal = [&](const std::string &why) {
        LOG.error("upload: session %s failed: %s", session.c_str(), why.c_str());
        u["state"] = "failed";
        u["error"] = why;
        write_json(sdir / "upload.json", u);
        std::lock_guard<std::mutex> lock(snap_mutex_);
        snap_.last_error = session + ": " + why;
        return true;   // done with it
    };
    auto retry = [&](const std::string &why) {
        LOG.warn("upload: session %s: %s; retrying later", session.c_str(), why.c_str());
        std::lock_guard<std::mutex> lock(snap_mutex_);
        snap_.last_error = session + ": " + why;
        return false;
    };
    auto stage = [&](const char *name) {
        set_stage(name, 0);
        u["stage"] = name;
        write_json(sdir / "upload.json", u);
    };
    if (!s.is_object() || s.value("status", "") != "complete" || !s.contains("consent") || s["consent"].is_null())
        return fatal("not a complete, consented session");
    const auto keep_going = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        return !stop_;
    };
    u["state"] = "uploading";
    const fs::path tar = sdir / "package.tar", bin = sdir / "package.tar.bin";
    std::error_code ec;

    // 1. The package.
    if (!fs::exists(tar, ec)) {
        stage("packaging");
        PackagerOptions po = package_options_;
        po.cancelled = [&] { return !keep_going() || (paused_ && paused_()); };
        po.progress = [this](double p) { set_stage("packaging", float(p)); };
        PackageResult pr;
        std::thread t = start_thread(package_spec_, [&] { pr = packager_(sdir, po); });
        t.join();
        if (!pr.ok)
            return pr.retry ? retry("packaging: " + pr.error) : fatal("packaging: " + pr.error);
        u["package"] = {{"bytes", fs::file_size(tar, ec)}, {"made_unix_ns", pr.package.value("made_unix_ns", uint64_t(0))}};
        u.erase("object");
        fs::remove(bin, ec);
    }

    // 2. The encrypted object.
    if (!u.contains("object") || !fs::exists(bin, ec)) {
        stage("encrypting");
        const fs::path pem = sdir / ".upload-key.pem";
        std::ofstream(pem) << public_key_ << (public_key_.ends_with('\n') ? "" : "\n");
        std::string err;
        const bool sealed = make_envelope(tar, bin, pem, &err);
        fs::remove(pem, ec);
        if (!sealed)
            return retry("encrypting: " + err);
        u["object"] = object_name(contributor_id(), s.value("routine", ""), session, time(nullptr));
        u["object_bytes"] = fs::file_size(bin, ec);
        write_json(sdir / "upload.json", u);
    }

    // 3. One signed PUT.
    stage("uploading");
    const std::string object = u["object"];
    Http put{"PUT", endpoint_ + "/" + bucket_ + "/" + object, bin.string(),
             {{"Content-Type", "application/octet-stream"}}, "aws:amz:" + region_ + ":s3",
             access_key_ + ":" + secret_key_};
    const Response r = transport_(put, [&](float p) {
        if (p >= 0)
            set_stage("uploading", p);
        return keep_going();
    });
    switch (classify(r.status)) {
    case Outcome::Fatal:
        return fatal("the bucket refused the upload (" + std::to_string(r.status) + "): " + trim(r.body).substr(0, 300));
    case Outcome::Retry: return retry(r.status ? "bucket error " + std::to_string(r.status) : r.error);
    case Outcome::Ok: break;
    }
    const uint64_t bytes = u.value("object_bytes", uint64_t(0));
    u["state"] = "done";
    u.erase("stage");
    u["uploaded_unix_ns"] = wall_ns();
    fs::remove(bin, ec);   // package.tar stays: what was sent, readable
    write_json(sdir / "upload.json", u);
    LOG.info("upload: session %s uploaded as %s (%.1f MB)", session.c_str(), object.c_str(), double(bytes) / 1e6);
    return true;
}

void Uploader::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(5), [&] { return stop_ || kick_; });
            if (stop_)
                return;
            kick_ = false;
        }
        if (paused_ && paused_())
            continue;   // a recording owns the disk and the CPU
        // The oldest queued or interrupted session that isn't waiting out a retry.
        std::vector<fs::path> todo;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(dir_, ec)) {
            const json u = read_json(e.path() / "upload.json");
            if (u.is_object() && (u.value("state", "") == "queued" || u.value("state", "") == "uploading"))
                todo.push_back(e.path());
        }
        std::sort(todo.begin(), todo.end());
        const uint64_t now = mono_ns();
        for (const auto &sdir : todo) {
            const std::string session = sdir.filename().string();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (auto it = retry_at_.find(session); it != retry_at_.end() && it->second > now)
                    continue;
            }
            {
                std::lock_guard<std::mutex> lock(snap_mutex_);
                snap_.active = session;
                snap_.stage.clear();
                snap_.progress = 0;
            }
            const bool finished = upload_one(sdir);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (finished) {
                    retry_at_.erase(session);
                    backoff_.erase(session);
                } else {
                    uint64_t &b = backoff_[session];
                    b = b ? std::min<uint64_t>(b * 2, 1800000000000ull) : 30000000000ull;
                    retry_at_[session] = mono_ns() + b;
                }
            }
            {
                std::lock_guard<std::mutex> lock(snap_mutex_);
                snap_.active.clear();
                snap_.stage.clear();
                snap_.progress = 0;
            }
            count_pending();
            break;   // one per wakeup: re-check paused() and stop between sessions
        }
        if (!todo.empty()) {
            // More may be ready now: if so, don't wait before the next one.
            std::lock_guard<std::mutex> lock(mutex_);
            bool any_ready = false;
            for (const auto &sdir : todo) {
                auto it = retry_at_.find(sdir.filename().string());
                any_ready |= it == retry_at_.end() || it->second <= mono_ns();
            }
            kick_ = any_ready;
        }
    }
}

Uploader::Response Uploader::curl_transport(const Http &req, const Progress &progress) {
    Response res;
    static std::atomic<int> counter{0};
    const fs::path tmp = fs::temp_directory_path() /
                         ("babble-upload-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
    const fs::path body_out = tmp.string() + ".out", body_in = tmp.string() + ".cfg";
    std::vector<std::string> argv = {"curl", "-S", "-#", "-X", req.method, "-o", body_out.string(), "-w",
                                     "%{http_code}", "-A", "babble-frame-runtime",
                                     // A stalled transfer gives up after a minute below 1 KB/s.
                                     "--speed-time", "60", "--speed-limit", "1024", "--connect-timeout", "20"};
    for (const auto &[k, v] : req.headers)
        argv.insert(argv.end(), {"-H", k + ": " + v});
    if (!req.body_file.empty())
        argv.insert(argv.end(), {"--upload-file", req.body_file});
    else
        argv.insert(argv.end(), {"--max-time", "60"});
    if (!req.sigv4.empty()) {
        // The credentials go in a config file, not on the command line.
        std::string quoted;
        for (char c : req.credentials) {
            if (c == '"' || c == '\\')
                quoted += '\\';
            quoted += c;
        }
        const int fd = ::open(body_in.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        const std::string line = "user = \"" + quoted + "\"\n";
        const bool ok = fd >= 0 && ::write(fd, line.data(), line.size()) == ssize_t(line.size());
        if (fd >= 0)
            ::close(fd);
        if (!ok) {
            res.error = "cannot write curl's config";
            return res;
        }
        argv.insert(argv.end(), {"--aws-sigv4", req.sigv4, "-K", body_in.string()});
    }
    argv.push_back(req.url);
    Process p;
    Process::Options o;
    o.stdout_pipe = true;
    o.stderr_pipe = true;
    std::string out, err;
    if (!p.spawn(argv, o, &res.error))
        return res;
    while (p.running()) {
        if (progress && !progress(-1)) {
            p.terminate(500);
            std::error_code ec;
            fs::remove(body_out, ec);
            fs::remove(body_in, ec);
            res.error = "cancelled";
            return res;
        }
        out += p.read_stdout();
        std::string e = p.read_stderr();
        if (progress && !e.empty()) {
            // curl's bar ends each redraw with "NN.N%".
            const size_t pct = e.rfind('%');
            if (pct != std::string::npos) {
                size_t start = pct;
                while (start > 0 && (isdigit(static_cast<unsigned char>(e[start - 1])) || e[start - 1] == '.'))
                    start--;
                const float v = start < pct ? strtof(e.substr(start, pct - start).c_str(), nullptr) : -1.0f;
                if (v >= 0 && v <= 100)
                    progress(v / 100.0f);
            }
        }
        err += e;
        sleep_ns(100000000ull);
    }
    out += p.read_stdout();
    err += p.read_stderr();
    res.status = atoi(trim(out).c_str());
    if (std::ifstream f(body_out); f)
        res.body.assign(std::istreambuf_iterator<char>(f), {});
    std::error_code ec;
    fs::remove(body_out, ec);
    fs::remove(body_in, ec);
    if (res.status == 0) {
        // The last line without the progress bar noise.
        const size_t nl = trim(err).rfind('\n');
        res.error = trim(nl == std::string::npos ? err : err.substr(nl + 1));
        if (res.error.empty())
            res.error = "no response";
    }
    return res;
}

} // namespace babble::capture
