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

// Uploads consented recording sessions to Babble's dataset bucket, in the
// background, resuming across restarts. Per session (docs/dataset-format.md):
//
//   1. package: make_package() turns the session into package.tar (H.265 from
//      the hardware encoder, stills, metadata), on the recorder's cpus;
//   2. encrypt: package.tar.bin, the envelope Baballonia uses
//      ([i32 LE wrapped-key length][RSA-4096 OAEP-SHA256 wrapped AES key]
//      [16 B IV][AES-256-CBC ciphertext]), so only the key holder can read it;
//   3. upload: one PUT to the S3-compatible bucket (Garage), signed with
//      SigV4, as {contributor}_{yyyyMMdd_HHmmss}_frame-{routine}-v2_{session}.tar.bin.
//
// openssl and curl are spawned; nothing new is linked. 4xx is permanent (the
// session is marked failed); 5xx and network errors are retried with
// backoff, from 30 s up to 30 min. A missing or busy hardware encoder is
// retried the same way.
//
// State per session lives in its folder's upload.json: queued | uploading |
// done | failed, the stage reached, and the object name. Nothing runs while a
// recording is (paused() true); packaging stops and starts again later if a
// recording begins mid-way.
class Uploader {
public:
    // An HTTP request through curl: method, url, a file to send, extra
    // headers, and SigV4 signing (provider string for curl's --aws-sigv4,
    // credentials as "key:secret"). Returns the HTTP status (0 = no response)
    // and the response body. `progress` is called with 0..1 while a file goes
    // out and with -1 now and then; returning false aborts the request.
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
        int pending = 0;           // queued or uploading sessions
        std::string active;        // the session being worked on
        std::string stage;         // packaging | encrypting | uploading
        float progress = 0;        // of the active stage
        std::string last_error;
    };

    Uploader(const nlohmann::json &cfg, std::filesystem::path captures_dir, const nlohmann::json &threads,
             std::function<bool()> paused, Transport transport = {}, Packager packager = {});
    ~Uploader();

    void start();
    bool available() const;
    Result queue(const std::string &session);   // main thread
    Snapshot snapshot() const;

    // Pure pieces, for the tests.
    enum class Outcome { Ok, Retry, Fatal };
    static Outcome classify(int http_status);
    static std::string object_name(const std::string &contributor, const std::string &routine,
                                   const std::string &session, time_t when);
    // Encrypts `in` into `out` (written whole or not at all) for the holder of
    // the private key matching public_key_pem (a PEM file).
    static bool make_envelope(const std::filesystem::path &in, const std::filesystem::path &out,
                              const std::filesystem::path &public_key_pem, std::string *err,
                              const std::string &openssl = "openssl");

    static Response curl_transport(const Http &req, const Progress &progress);

private:
    void run();
    bool upload_one(const std::filesystem::path &dir);   // false: retry later
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
    std::map<std::string, uint64_t> retry_at_;   // session -> mono ns
    std::map<std::string, uint64_t> backoff_;
};

} // namespace babble::capture
