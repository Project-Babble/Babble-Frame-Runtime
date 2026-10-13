#include "sink/record_sink.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sha256.h"
#include "platform/process.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include <sys/statvfs.h>

namespace {
const babble::log::Logger LOG("record");
} // namespace

namespace babble {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct RecordSink::Channel {
    std::string source;
    std::mutex m;   // everything up to the writer-only block
    uint32_t w = 0, h = 0;   // the recorded frame: both sides of a pair, side by side
    bool stereo = false;
    FrameOrigin origin = FrameOrigin::EyeL;
    std::vector<std::unique_ptr<uint8_t[]>> bufs;   // grown up to queue_frames, stable addresses
    std::vector<size_t> free_list;
    std::deque<Item> queue;
    SourceCounts counts;
    bool warned_size = false;
    uint64_t first_ns = 0, last_ns = 0;

    // Writer thread only.
    std::unique_ptr<Process> enc;
    FILE *stamps = nullptr;
    bool failed = false;
    std::string error;
    uint64_t written = 0;

    void reset() {
        std::lock_guard<std::mutex> lock(m);
        w = h = 0;
        stereo = false;
        bufs.clear();
        free_list.clear();
        queue.clear();
        counts = {};
        warned_size = false;
        first_ns = last_ns = 0;
        enc.reset();
        if (stamps)
            fclose(stamps);
        stamps = nullptr;
        failed = false;
        error.clear();
        written = 0;
    }
};

namespace {

// The default timestamp meaning per camera (see each source).
const char *timestamp_kind(FrameOrigin o) {
    switch (o) {
    case FrameOrigin::EyeL:
    case FrameOrigin::EyeR: return "detected";   // dsp_service has no frame timestamp: when the poll saw it
    case FrameOrigin::Face: return "arrival";    // UVC payloads carry none: when the frame arrived
    case FrameOrigin::MouthL:
    case FrameOrigin::MouthR: return "sensor";   // V4L2 buffer timestamp
    }
    return "?";
}

std::string replace_all(std::string s, const std::string &from, const std::string &to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
        s.replace(at, from.size(), to);
    return s;
}

bool write_json_file(const fs::path &path, const json &j) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            return false;
        f << j.dump(2) << '\n';
        if (!f)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

// The headset model (devicetree) and OS build, for splitting the corpus by
// hardware and software revision.
json read_device() {
    json d = json::object();
    std::ifstream model("/sys/firmware/devicetree/base/model");
    std::string m;
    if (std::getline(model, m, '\0') && !m.empty())
        d["model"] = m;
    json os = json::object();
    std::ifstream rel("/etc/os-release");
    for (std::string line; std::getline(rel, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
            val = val.substr(1, val.size() - 2);
        if (key == "ID" || key == "VERSION_ID" || key == "BUILD_ID" || key == "VARIANT_ID") {
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            os[key] = val;
        }
    }
    if (!os.empty())
        d["os"] = os;
    return d;
}

void put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = uint8_t(v >> (8 * i));
}
void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = uint8_t(v >> (8 * i));
}
void put_f32(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    put_u32(p, u);
}

constexpr size_t kMaxPendingEyeSamples = 4096;   // about 45 s of samples if the writer stalls

std::string session_stamp() {
    char buf[32];
    const time_t t = time(nullptr);
    tm local{};
    localtime_r(&t, &local);
    strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &local);
    return buf;
}

} // namespace

RecordSink::RecordSink(const json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "record"), "bbl-record")),
      dir_(paths::expand_home(cfg.value("dir", (paths::data_dir() / "captures").string()))),
      container_(cfg.value("container", "mkv")), queue_frames_(std::max<size_t>(1, cfg.value("queue_frames", 64u))),
      min_free_gb_(cfg.value("min_free_gb", 5.0)), version_(deps.version), profile_(deps.profile),
      device_(read_device()) {
    for (const auto &a : cfg.value("encoder", json::array()))
        if (a.is_string())
            encoder_.push_back(a.get<std::string>());
}

RecordSink::~RecordSink() { stop(); }

bool RecordSink::start() {
    if (thread_.joinable())
        return true;
    stop_thread_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    LOG.info("%s: recording to %s (%s)", name_.c_str(), dir_.c_str(),
             available() ? "encoder found" : "encoder not found: recording unavailable");
    return true;
}

void RecordSink::stop() {
    accepting_ = false;
    if (!thread_.joinable())
        return;
    stop_thread_ = true;
    cv_.notify_all();
    thread_.join();
}

bool RecordSink::available() const { return !encoder_.empty() && Process::find_in_path(encoder_[0]).has_value(); }

bool RecordSink::low_disk() const {
    struct statvfs st{};
    std::error_code ec;
    fs::path probe = dir_;
    while (!probe.empty() && !fs::exists(probe, ec))
        probe = probe.parent_path();
    if (probe.empty() || statvfs(probe.c_str(), &st) != 0)
        return false;   // can't tell: let the write fail instead
    return double(st.f_bavail) * double(st.f_frsize) < min_free_gb_ * 1e9;
}

RecordSink::Channel *RecordSink::channel(const std::string &source) {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    auto &c = channels_[source];
    if (!c) {
        c = std::make_unique<Channel>();
        c->source = source;
    }
    return c.get();   // channels are never removed, only reset between sessions
}

void RecordSink::tap_frames(const std::string &source, const StereoFrame &sf) {
    if (!accepting_.load(std::memory_order_relaxed) || !sf.left)
        return;
    const Frame &l = *sf.left;
    const Frame *r = sf.right.get();
    const uint32_t w = l.width + (r ? r->width : 0), h = l.height;
    Channel *ch = channel(source);
    bool warn = false;
    {
        std::lock_guard<std::mutex> lock(ch->m);
        if (ch->w == 0) {
            ch->w = w;
            ch->h = h;
            ch->stereo = r != nullptr;
            ch->origin = l.origin;
        }
        if (w != ch->w || h != ch->h || (r && r->height != h) || ch->stereo != (r != nullptr)) {
            ch->counts.size_changed++;
            warn = !ch->warned_size;
            ch->warned_size = true;
        } else {
            size_t idx;
            if (!ch->free_list.empty()) {
                idx = ch->free_list.back();
                ch->free_list.pop_back();
            } else if (ch->bufs.size() < queue_frames_) {
                ch->bufs.push_back(std::make_unique<uint8_t[]>(size_t(w) * h));
                idx = ch->bufs.size() - 1;
            } else {
                ch->counts.dropped++;   // the writer is behind: drop rather than block the camera
                return;
            }
            uint8_t *dst = ch->bufs[idx].get();
            for (uint32_t y = 0; y < h; y++) {
                memcpy(dst + size_t(y) * w, l.data + size_t(y) * l.stride, l.width);
                if (r)
                    memcpy(dst + size_t(y) * w + l.width, r->data + size_t(y) * r->stride, r->width);
            }
            ch->queue.push_back({idx, {sf.capture_ns, l.seq, sf.skew_ns}});
            ch->counts.frames++;
            if (!ch->first_ns)
                ch->first_ns = sf.capture_ns;
            ch->last_ns = sf.capture_ns;
        }
    }
    if (warn)
        LOG.warn("%s: %s frame size changed to %ux%u during the session; dropping those frames", name_.c_str(),
                 source.c_str(), w, h);
    cv_.notify_one();
}

RecordSink::StartResult RecordSink::start_session(const SessionSpec &spec) {
    StartResult res;
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (open_) {
        res.error = "busy";
        res.message = "a recording is already running";
        return res;
    }
    if (!thread_.joinable() || !available()) {
        res.error = "unavailable";
        res.message = encoder_.empty() ? "no encoder configured" : encoder_[0] + " not found";
        return res;
    }
    if (low_disk()) {
        res.error = "disk";
        char buf[96];
        snprintf(buf, sizeof(buf), "less than %.0f GB free for recordings", min_free_gb_);
        res.message = buf;
        return res;
    }
    std::string id = session_stamp();
    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::path sdir = dir_ / id;
    for (int n = 2; fs::exists(sdir, ec); n++)
        sdir = dir_ / (id + "-" + std::to_string(n));
    id = sdir.filename().string();
    if (!fs::create_directories(sdir, ec)) {
        res.error = "io";
        res.message = "cannot create " + sdir.string() + ": " + ec.message();
        return res;
    }
    {
        std::lock_guard<std::mutex> cl(channels_mutex_);
        for (auto &[name, c] : channels_)
            c->reset();
    }
    open_ = true;
    stop_requested_ = false;
    stop_reason_.clear();
    finished_.reset();
    id_ = id;
    session_dir_ = sdir;
    spec_ = spec;
    started_ns_ = mono_ns();
    ended_ns_ = 0;
    realtime_offset_ns_ = int64_t(wall_ns()) - int64_t(started_ns_);
    routine_sha256_.clear();
    if (spec.routine_def.is_object()) {
        const fs::path rf = sdir / "routine.json";
        if (!write_json_file(rf, spec.routine_def)) {
            open_ = false;
            res.error = "io";
            res.message = "cannot write " + rf.string();
            return res;
        }
        routine_sha256_ = Sha256::file_hex(rf).value_or("");
    }
    {
        std::lock_guard<std::mutex> el(eye_mutex_);
        eye_pending_.clear();
        eye_dropped_ = 0;
    }
    eye_records_ = 0;   // the writer is idle until the session opens
    eye_layout_ = 0;
    write_manifest("recording", "", "");
    accepting_ = true;
    eye_accepting_ = spec.valve_eye;
    cv_.notify_one();
    LOG.info("%s: session %s started (%s)", name_.c_str(), id.c_str(),
             spec.routine.empty() ? "no routine" : spec.routine.c_str());
    res.ok = true;
    res.id = id;
    res.dir = sdir.string();
    return res;
}

bool RecordSink::add_label(const std::string &label, uint64_t start_ns, uint64_t end_ns, std::string *err,
                           const json &extra) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (!open_ || stop_requested_) {
        if (err)
            *err = "not recording";
        return false;
    }
    if (end_ns < start_ns) {
        if (err)
            *err = "end_ns is before start_ns";
        return false;
    }
    std::ofstream f(session_dir_ / "labels.jsonl", std::ios::app);
    json line = {{"label", label}, {"start_ns", start_ns}, {"end_ns", end_ns}};
    if (extra.is_object())
        for (const auto &[k, v] : extra.items())
            if (!line.contains(k))
                line[k] = v;
    f << line.dump() << '\n';
    if (!f) {
        if (err)
            *err = "cannot write labels.jsonl";
        return false;
    }
    return true;
}

bool RecordSink::stop_session(const std::string &reason) {
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!open_ || stop_requested_)
            return false;
        stop_requested_ = true;
        stop_reason_ = reason;
        accepting_ = false;
        eye_accepting_ = false;
    }
    cv_.notify_one();
    LOG.info("%s: session %s stopping%s%s", name_.c_str(), id_.c_str(), reason.empty() ? "" : ": ", reason.c_str());
    return true;
}

std::optional<RecordSink::Finished> RecordSink::take_finished() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto f = std::move(finished_);
    finished_.reset();
    return f;
}

RecordSink::Status RecordSink::status() const {
    Status s;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        s.recording = open_;
        s.finishing = open_ && stop_requested_;
        s.id = id_;
        s.started_ns = started_ns_;
        if (!open_)
            return s;
    }
    std::lock_guard<std::mutex> cl(channels_mutex_);
    for (const auto &[name, c] : channels_) {
        std::lock_guard<std::mutex> lock(c->m);
        if (c->w)
            s.sources[name] = c->counts;
    }
    return s;
}

void RecordSink::add_eye_sample(const eye_server::Sample &s) {
    if (!eye_accepting_.load(std::memory_order_relaxed))
        return;
    eye_layout_.store(s.layout, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(eye_mutex_);
    if (eye_pending_.size() >= kMaxPendingEyeSamples) {
        eye_dropped_++;
        return;
    }
    eye_pending_.push_back(s);
}

void RecordSink::encode_eye_record(const eye_server::Sample &s, uint8_t out[kEyeRecordBytes]) {
    put_u64(out + 0, s.received_ns);
    uint64_t t;
    memcpy(&t, &s.sample_time, 8);
    put_u64(out + 8, t);
    for (int e = 0; e < 2; e++)
        for (int k = 0; k < 3; k++) {
            put_f32(out + 16 + 4 * (3 * e + k), s.gaze[e][k]);
            put_f32(out + 40 + 4 * (3 * e + k), s.pre_fusion_gaze[e][k]);
        }
    for (int k = 0; k < 3; k++)
        put_f32(out + 64 + 4 * k, s.fixation[k]);
    put_f32(out + 76, s.openness[0]);
    put_f32(out + 80, s.openness[1]);
    put_u32(out + 84, 1);   // valid: the client only publishes valid records
}

void RecordSink::write_eye_samples() {
    std::vector<eye_server::Sample> batch;
    {
        std::lock_guard<std::mutex> lock(eye_mutex_);
        batch.swap(eye_pending_);
    }
    if (batch.empty())
        return;
    if (!eye_file_) {
        eye_file_ = fopen((session_dir_ / "valve_eye.bin").c_str(), "wb");
        if (!eye_file_) {
            LOG.error("%s: cannot write valve_eye.bin: %s", name_.c_str(), strerror(errno));
            eye_accepting_ = false;
            return;
        }
    }
    uint8_t rec[kEyeRecordBytes];
    for (const auto &s : batch) {
        encode_eye_record(s, rec);
        if (fwrite(rec, sizeof(rec), 1, eye_file_) == 1)
            eye_records_++;
    }
}

bool RecordSink::open_encoder(Channel &ch) {
    uint32_t w, h;
    {
        std::lock_guard<std::mutex> lock(ch.m);
        w = ch.w;
        h = ch.h;
    }
    const fs::path out = session_dir_ / (ch.source + "." + container_);
    const auto fps_it = spec_.source_fps.find(ch.source);
    char fps[32];
    snprintf(fps, sizeof(fps), "%g", fps_it != spec_.source_fps.end() && fps_it->second > 0 ? fps_it->second : 30.0);
    std::vector<std::string> argv;
    for (const auto &a : encoder_)
        argv.push_back(replace_all(replace_all(replace_all(replace_all(a, "{w}", std::to_string(w)), "{h}",
                                                           std::to_string(h)),
                                               "{fps}", fps),
                                   "{out}", out.string()));
    Process::Options o;
    o.stdin_pipe = true;
    o.stderr_file = (session_dir_ / ("encoder-" + ch.source + ".log")).string();
    o.stdout_file = o.stderr_file;
    ch.enc = std::make_unique<Process>();
    std::string err;
    if (!ch.enc->spawn(argv, o, &err)) {
        ch.failed = true;
        ch.error = "encoder: " + err;
        LOG.error("%s: %s: cannot start the encoder: %s", name_.c_str(), ch.source.c_str(), err.c_str());
        return false;
    }
    ch.stamps = fopen((session_dir_ / (ch.source + ".stamps")).c_str(), "wb");
    if (!ch.stamps) {
        ch.failed = true;
        ch.error = std::string("cannot write ") + ch.source + ".stamps: " + strerror(errno);
        LOG.error("%s: %s", name_.c_str(), ch.error.c_str());
        return false;
    }
    LOG.info("%s: %s: encoding %ux%u at %s fps to %s", name_.c_str(), ch.source.c_str(), w, h, fps, out.c_str());
    return true;
}

bool RecordSink::write_items(Channel &ch) {
    bool any = false;
    for (;;) {
        Item item;
        const uint8_t *data;
        size_t bytes;
        {
            std::lock_guard<std::mutex> lock(ch.m);
            if (ch.queue.empty())
                break;
            item = ch.queue.front();
            ch.queue.pop_front();
            data = ch.bufs[item.buf].get();
            bytes = size_t(ch.w) * ch.h;
        }
        any = true;
        if (!ch.failed && !ch.enc)
            open_encoder(ch);
        if (!ch.failed) {
            if (ch.enc->write_stdin(data, bytes, 2000)) {
                uint8_t rec[24];
                const uint64_t cap = item.stamp.capture_ns, seq = item.stamp.seq;
                const uint64_t skew = uint64_t(item.stamp.skew_ns);
                for (int i = 0; i < 8; i++) {
                    rec[i] = uint8_t(cap >> (8 * i));
                    rec[8 + i] = uint8_t(seq >> (8 * i));
                    rec[16 + i] = uint8_t(skew >> (8 * i));
                }
                fwrite(rec, sizeof(rec), 1, ch.stamps);
                ch.written++;
            } else {
                ch.failed = true;
                ch.error = "the encoder stopped taking frames (see encoder-" + ch.source + ".log)";
                LOG.error("%s: %s: %s", name_.c_str(), ch.source.c_str(), ch.error.c_str());
            }
        }
        std::lock_guard<std::mutex> lock(ch.m);
        ch.free_list.push_back(item.buf);
    }
    return any;
}

void RecordSink::run() {
    uint64_t next_disk_check = 0;
    while (!stop_thread_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }
        bool open, stopping;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            open = open_;
            stopping = stop_requested_;
        }
        if (!open)
            continue;
        std::vector<Channel *> chans;
        {
            std::lock_guard<std::mutex> cl(channels_mutex_);
            for (auto &[name, c] : channels_)
                chans.push_back(c.get());
        }
        bool wrote = true;
        while (wrote && !stop_thread_) {   // keep draining while frames keep coming
            wrote = false;
            for (Channel *c : chans)
                wrote |= write_items(*c);
        }
        write_eye_samples();
        const uint64_t now = mono_ns();
        if (!stopping && now >= next_disk_check) {
            next_disk_check = now + 5000000000ull;
            if (low_disk()) {
                char buf[96];
                snprintf(buf, sizeof(buf), "disk: less than %.0f GB free", min_free_gb_);
                LOG.warn("%s: %s; stopping the recording", name_.c_str(), buf);
                stop_session(buf);
                stopping = true;
            }
        }
        if (stopping) {
            for (Channel *c : chans)   // frames that came in while stop was requested
                write_items(*c);
            finish_session(10000);
        }
    }
    bool open;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        open = open_;
        if (open && stop_reason_.empty())
            stop_reason_ = "the runtime stopped";
        stop_requested_ = true;
        eye_accepting_ = false;
    }
    if (open)
        finish_session(1000);   // shutdown: queued frames are dropped; mkv tolerates the cut
}

void RecordSink::finish_session(int encoder_wait_ms) {
    std::vector<Channel *> chans;
    {
        std::lock_guard<std::mutex> cl(channels_mutex_);
        for (auto &[name, c] : channels_)
            chans.push_back(c.get());
    }
    for (Channel *c : chans)
        if (c->enc)
            c->enc->close_stdin();
    const uint64_t deadline = mono_ns() + uint64_t(encoder_wait_ms) * 1000000ull;
    std::string errors;
    for (Channel *c : chans) {
        if (c->enc) {
            const uint64_t now = mono_ns();
            if (!c->enc->wait(int(now < deadline ? (deadline - now) / 1000000ull : 0))) {
                c->enc->terminate(200);
                if (!c->failed) {
                    c->failed = true;
                    c->error = "the encoder didn't finish in time";
                }
            } else if (c->enc->exit_code().value_or(-1) != 0 && !c->failed) {
                c->failed = true;
                c->error = "the encoder failed (see encoder-" + c->source + ".log)";
            }
        }
        if (c->stamps) {
            fclose(c->stamps);
            c->stamps = nullptr;
        }
        if (c->failed)
            errors += (errors.empty() ? "" : "; ") + c->source + ": " + c->error;
    }
    write_eye_samples();
    if (eye_file_) {
        fclose(eye_file_);
        eye_file_ = nullptr;
    }
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        reason = stop_reason_;
        ended_ns_ = mono_ns();
    }
    const std::string status = errors.empty() ? "complete" : "failed";
    write_manifest(status, errors, reason);

    Finished f;
    f.status = status;
    f.error = errors;
    f.stop_reason = reason;
    for (Channel *c : chans) {
        std::lock_guard<std::mutex> lock(c->m);
        if (c->w)
            f.sources[c->source] = c->counts;
    }
    std::lock_guard<std::mutex> lock(session_mutex_);
    f.id = id_;
    f.dir = session_dir_.string();
    std::string note = reason;
    if (!errors.empty())
        note += (note.empty() ? "" : "; ") + errors;
    LOG.info("%s: session %s %s%s%s", name_.c_str(), id_.c_str(), status.c_str(), note.empty() ? "" : ": ",
             note.c_str());
    finished_ = std::move(f);
    open_ = false;
    stop_requested_ = false;
}

void RecordSink::write_manifest(const std::string &status, const std::string &error, const std::string &stop_reason) {
    json m;
    m["format"] = 2;
    m["id"] = id_;
    m["status"] = status;
    if (!error.empty())
        m["error"] = error;
    m["stop_reason"] = stop_reason.empty() ? json(nullptr) : json(stop_reason);   // null while recording
    m["runtime"] = {{"version", version_}, {"profile", profile_}};
    m["device"] = device_;
    m["routine"] = spec_.routine;
    if (!routine_sha256_.empty()) {
        m["routine_file"] = "routine.json";
        m["routine_sha256"] = routine_sha256_;
    }
    m["meta"] = spec_.meta;
    m["consent"] = spec_.consent;
    m["context"] = spec_.context;
    m["started_ns"] = started_ns_;
    m["ended_ns"] = ended_ns_ ? json(ended_ns_) : json(nullptr);
    m["realtime_offset_ns"] = realtime_offset_ns_;
    m["clock"] = "CLOCK_MONOTONIC nanoseconds; add realtime_offset_ns for Unix time";
    m["stamps_format"] = "per frame, 24 bytes little-endian: u64 capture_ns, u64 seq, i64 skew_ns";
    m["labels"] = "labels.jsonl";
    m["encoder"] = encoder_;
    json sources = json::object();
    std::vector<Channel *> chans;
    {
        std::lock_guard<std::mutex> cl(channels_mutex_);
        for (auto &[name, c] : channels_)
            chans.push_back(c.get());
    }
    for (Channel *c : chans) {
        std::lock_guard<std::mutex> lock(c->m);
        if (!c->w)
            continue;
        json s;
        const std::string file = c->source + "." + container_, stamps = c->source + ".stamps";
        std::error_code ec;
        s["file"] = file;
        s["stamps"] = stamps;
        s["bytes"] = fs::exists(session_dir_ / file, ec) ? json(fs::file_size(session_dir_ / file, ec)) : json(0);
        s["width"] = c->w;
        s["height"] = c->h;
        s["layout"] = c->stereo ? "stereo_lr" : "mono";
        s["origin"] = to_string(c->origin);
        s["timestamp_kind"] = timestamp_kind(c->origin);
        const auto fps_it = spec_.source_fps.find(c->source);
        s["nominal_fps"] = fps_it != spec_.source_fps.end() ? fps_it->second : 0.0;
        s["frames"] = c->written;
        s["captured"] = c->counts.frames;
        s["dropped"] = c->counts.dropped;
        s["size_changed"] = c->counts.size_changed;
        s["measured_fps"] = c->counts.frames > 1 && c->last_ns > c->first_ns
                                ? double(c->counts.frames - 1) * 1e9 / double(c->last_ns - c->first_ns)
                                : 0.0;
        if (c->failed)
            s["error"] = c->error;
        sources[c->source] = s;
    }
    m["sources"] = sources;
    if (spec_.valve_eye) {
        const uint32_t layout = eye_layout_.load();
        uint64_t dropped;
        {
            std::lock_guard<std::mutex> el(eye_mutex_);
            dropped = eye_dropped_;
        }
        m["valve_eye"] = {{"file", "valve_eye.bin"},
                          {"records", eye_records_},
                          {"dropped", dropped},
                          {"layout", layout ? json(layout) : json(nullptr)},   // null: no samples came
                          {"record_format", "88 bytes little-endian: u64 received_ns, f64 sample_time, "
                                            "f32 gaze[2][3], f32 pre_fusion_gaze[2][3], f32 fixation[3], "
                                            "f32 openness[2], u32 flags (bit 0 valid); index 0 = left eye"}};
    }
    if (!write_json_file(session_dir_ / "session.json", m))
        LOG.error("%s: cannot write %s/session.json", name_.c_str(), session_dir_.c_str());
}

} // namespace babble
