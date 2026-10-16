#pragma once

#include "capture/capture_protocol.h"
#include "capture/packager.h"
#include "core/threading.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace babble::capture {

// Packages, encrypts and uploads consented sessions to the dataset bucket, and continues after a restart.
class Uploader {
public:
    // An HTTP request for curl, with optional SigV4 signing and credentials in the form "key:secret".
    struct Http {
        std::string method, url, body_file;
        std::map<std::string, std::string> headers;
        std::string sigv4, credentials;
    };
    struct Response {
        int status = 0;
        std::string body, error;
    };
    using Progress = std::function<bool(float)>;
    using Transport = std::function<Response(const Http &, const Progress &progress)>;
    using Packager = std::function<PackageResult(const std::filesystem::path &session, const PackagerOptions &)>;

    struct Snapshot {
        int pending = 0;           // The number of queued or active sessions.
        std::string active;        // The session that the uploader works on now.
        std::string stage;         // The current stage of the active session.
        float progress = 0;        // The progress of the active stage.
        std::string last_error;
    };

    Uploader(const nlohmann::json &cfg, std::filesystem::path captures_dir, const nlohmann::json &threads,
             std::function<bool()> paused, Transport transport = {}, Packager packager = {});
    ~Uploader();

    void start();
    bool available() const;
    Result queue(const std::string &session);   // Call this only from the main thread.
    Snapshot snapshot() const;

    enum class Outcome { Ok, Retry, Fatal };
    static Outcome classify(int http_status);
    static std::string object_name(const std::string &contributor, const std::string &routine,
                                   const std::string &session, time_t when);
    // Encrypts `in` into `out` for the holder of the private key that matches the public_key_pem file.
    static bool make_envelope(const std::filesystem::path &in, const std::filesystem::path &out,
                              const std::filesystem::path &public_key_pem, std::string *err,
                              const std::string &openssl = "openssl");

    static Response curl_transport(const Http &req, const Progress &progress);

private:
    void run();
    bool upload_one(const std::filesystem::path &dir);   // Returns false if the session must be tried again later.
    std::string contributor_id();
    void count_pending();
    void set_stage(const std::string &stage, float progress);

    std::string endpoint_, bucket_, region_, access_key_, secret_key_, public_key_;
    std::filesystem::path dir_;
    ThreadSpec thread_spec_, package_spec_;
    std::function<bool()> paused_;
    Transport transport_;
    Packager packager_;
    PackagerOptions package_options_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool kick_ = false;

    mutable std::mutex snap_mutex_;
    Snapshot snap_;
    std::map<std::string, uint64_t> retry_at_;   // This maps a session to its retry time in monotonic ns.
    std::map<std::string, uint64_t> backoff_;
};

}
