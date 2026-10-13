#include <doctest/doctest.h>

#include "capture/app_installer.h"
#include "capture/github_release.h"
#include "capture/uploader.h"
#include "core/sha256.h"
#include "platform/process.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

using namespace babble;
using namespace babble::capture;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json release(const std::string &tag, bool draft, bool pre, std::vector<std::string> assets) {
    json a = json::array();
    for (const auto &n : assets)
        a.push_back({{"name", n}, {"browser_download_url", "https://example/" + tag + "/" + n}, {"size", 10}});
    return {{"tag_name", tag}, {"draft", draft}, {"prerelease", pre}, {"assets", a}};
}

template <typename Pred> bool wait_for(Pred p, int ms = 10000) {
    for (int i = 0; i < ms / 20; i++) {
        if (p())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return p();
}

void write(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

} // namespace

TEST_CASE("github releases: the newest complete, allowed release wins") {
    const std::string tar3 = "babble-dataset-capture-0.3.0-linuxarm64.tar.gz";
    const std::string tar2 = "babble-dataset-capture-0.2.0-linuxarm64.tar.gz";
    const std::string tar1 = "babble-dataset-capture-0.1.0-linuxarm64.tar.gz";
    json api = json::array({
        release("v0.4.0", true, false, {"x.tar.gz"}),                                         // draft
        release("v0.3.0", false, true, {tar3, tar3 + ".sha256", "manifest.json"}),            // prerelease
        release("v0.2.5", false, false, {"babble-dataset-capture-0.2.5-linuxarm64.tar.gz"}),  // incomplete
        release("v0.2.0", false, false, {tar2, tar2 + ".sha256", "manifest.json"}),
        release("v0.1.0", false, false, {tar1, tar1 + ".sha256", "manifest.json"}),
    });
    const auto rels = parse_releases(api);
    REQUIRE(rels.size() == 5);
    auto c = candidates(rels, "babble-dataset-capture-", false);
    REQUIRE(c.size() == 2);
    CHECK(c[0].release.tag == "v0.2.0");
    CHECK(c[0].tarball.name == tar2);
    CHECK(c[0].sha256.name == tar2 + ".sha256");
    auto withpre = candidates(rels, "babble-dataset-capture-", true);
    REQUIRE(withpre.size() == 3);
    CHECK(withpre[0].release.tag == "v0.3.0");
    CHECK(parse_releases(json::object()).empty());
}

TEST_CASE("manifests and checksum files are checked") {
    std::string err;
    auto m = parse_manifest(json::parse(R"({"name":"DC","version":"0.1.0","protocol":1,"entry":"bin/app"})"), &err);
    REQUIRE(m);
    CHECK(m->entry == "bin/app");
    CHECK_FALSE(parse_manifest(json::parse(R"({"version":"1","protocol":1,"entry":"/bin/sh"})"), &err));
    CHECK_FALSE(parse_manifest(json::parse(R"({"version":"1","protocol":1,"entry":"bin/../../x"})"), &err));
    CHECK_FALSE(parse_manifest(json::parse(R"({"version":"1","entry":"bin/app"})"), &err));
    CHECK_FALSE(parse_manifest(json::parse(R"({"protocol":1,"entry":"bin/app"})"), &err));

    const std::string h(64, 'a');
    CHECK(parse_sha256_file(h + "  file.tar.gz\n", "file.tar.gz") == h);
    CHECK(parse_sha256_file(std::string(64, 'B') + " *file.tar.gz\n", "file.tar.gz") == std::string(64, 'b'));
    CHECK(parse_sha256_file(h + "\n", "anything") == h);
    CHECK_FALSE(parse_sha256_file(h + "  other.tar.gz\n", "file.tar.gz"));
    CHECK_FALSE(parse_sha256_file("nothing here", "file.tar.gz"));
}

TEST_CASE("app installer: installs a release from file:// URLs, rejects a bad checksum, removes it") {
    if (!Process::find_in_path("curl") || !Process::find_in_path("tar"))
        return;   // the tools the installer drives
    const fs::path root = fs::temp_directory_path() / ("babble_apps_" + std::to_string(getpid()));
    fs::remove_all(root);
    const fs::path rel = root / "release", app = root / "app";
    const std::string manifest = R"({"name":"Dataset Capture","version":"0.2.0","protocol":1,"entry":"bin/app"})";
    write(app / "manifest.json", manifest);
    write(app / "bin" / "app", "#!/bin/sh\necho hi\n");
    chmod((app / "bin" / "app").c_str(), 0755);
    const std::string tarname = "babble-dataset-capture-0.2.0-linuxarm64.tar.gz";
    fs::create_directories(rel);
    REQUIRE(Process::run({"tar", "-czf", (rel / tarname).string(), "-C", app.string(), "."}, nullptr, nullptr, 20000) == 0);
    write(rel / "manifest.json", manifest);
    auto publish = [&](const std::string &digest) {
        write(rel / (tarname + ".sha256"), digest + "  " + tarname + "\n");
        json assets = json::array();
        for (const auto &n : {tarname, tarname + ".sha256", std::string("manifest.json")})
            assets.push_back({{"name", n},
                              {"browser_download_url", "file://" + (rel / n).string()},
                              {"size", fs::file_size(rel / n)}});
        write(rel / "releases.json",
              json::array({{{"tag_name", "v0.2.0"}, {"draft", false}, {"prerelease", false}, {"assets", assets}}}).dump());
    };
    const json cfg = {{"api_url", "file://" + (rel / "releases.json").string()}, {"dir", (root / "installed").string()}};

    publish(std::string(64, '0'));   // wrong digest
    {
        AppInstaller bad(cfg, 1, json::object());
        bad.start();
        bad.install();
        REQUIRE(wait_for([&] { return bad.snapshot().state == AppInstaller::State::Failed; }));
        CHECK(bad.snapshot().detail.find("checksum") != std::string::npos);
        CHECK_FALSE(fs::exists(root / "installed"));
    }

    publish(*Sha256::file_hex(rel / tarname));
    AppInstaller inst(cfg, 1, json::object());
    CHECK(inst.snapshot().state == AppInstaller::State::NotInstalled);
    inst.start();
    inst.check();
    REQUIRE(wait_for([&] { return inst.snapshot().state == AppInstaller::State::NotInstalled && !inst.snapshot().latest.empty(); }));
    CHECK(inst.snapshot().latest == "0.2.0");
    inst.install();
    REQUIRE(wait_for([&] { return inst.snapshot().state == AppInstaller::State::Installed; }));
    const auto snap = inst.snapshot();
    CHECK(snap.installed == "0.2.0");
    CHECK(snap.entry == (root / "installed" / "bin" / "app").string());
    CHECK(access(snap.entry.c_str(), X_OK) == 0);
    CHECK_FALSE(fs::exists(root / "installed.new"));
    CHECK_FALSE(fs::exists(root / "installed.download"));

    // A runtime speaking another protocol doesn't take it.
    std::string err;
    CHECK_FALSE(AppInstaller::validate_install(root / "installed", 2, &err));

    inst.remove();
    REQUIRE(wait_for([&] { return inst.snapshot().installed.empty(); }));
    CHECK_FALSE(fs::exists(root / "installed"));
    fs::remove_all(root);
}

TEST_CASE("uploader: classify and object names") {
    CHECK(Uploader::classify(200) == Uploader::Outcome::Ok);
    CHECK(Uploader::classify(204) == Uploader::Outcome::Ok);
    CHECK(Uploader::classify(400) == Uploader::Outcome::Fatal);
    CHECK(Uploader::classify(403) == Uploader::Outcome::Fatal);
    CHECK(Uploader::classify(429) == Uploader::Outcome::Retry);
    CHECK(Uploader::classify(503) == Uploader::Outcome::Retry);
    CHECK(Uploader::classify(0) == Uploader::Outcome::Retry);

    tm t{};
    t.tm_year = 126;
    t.tm_mon = 9;
    t.tm_mday = 12;
    t.tm_hour = 9;
    t.tm_min = 5;
    t.tm_sec = 7;
    t.tm_isdst = -1;
    CHECK(Uploader::object_name("c0ffee", "eye", "20261012-090500", mktime(&t)) ==
          "c0ffee_20261012_090507_frame-eye-v2_20261012-090500.tar.bin");
    CHECK(Uploader::object_name("c", "Mouth/X", "s", mktime(&t)) == "c_20261012_090507_frame-mouth_x-v2_s.tar.bin");
    CHECK(Uploader::object_name("c", "", "s", mktime(&t)) == "c_20261012_090507_frame-session-v2_s.tar.bin");
}

namespace {

// A throwaway RSA key pair in dir (priv.pem, pub.pem); false without openssl.
bool make_keys(const fs::path &dir) {
    if (!Process::find_in_path("openssl"))
        return false;
    fs::create_directories(dir);
    std::string out, err;
    return Process::run({"openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out",
                         (dir / "priv.pem").string()},
                        &out, &err, 60000) == 0 &&
           Process::run({"openssl", "pkey", "-in", (dir / "priv.pem").string(), "-pubout", "-out",
                         (dir / "pub.pem").string()},
                        &out, &err, 60000) == 0;
}

// Opens an envelope the way the ingest does: unwrap the key, then AES-256-CBC.
std::string open_envelope(const fs::path &obj, const fs::path &priv, const fs::path &work) {
    const std::string b = slurp(obj);
    REQUIRE(b.size() > 20);
    const uint32_t n = uint8_t(b[0]) | uint8_t(b[1]) << 8 | uint8_t(b[2]) << 16 | uint32_t(uint8_t(b[3])) << 24;
    REQUIRE(b.size() > 4 + n + 16);
    write(work / "wk", b.substr(4, n));
    write(work / "ct", b.substr(4 + n + 16));
    std::string key, err;
    REQUIRE(Process::run({"openssl", "pkeyutl", "-decrypt", "-inkey", priv.string(), "-pkeyopt",
                          "rsa_padding_mode:oaep", "-pkeyopt", "rsa_oaep_md:sha256", "-pkeyopt",
                          "rsa_mgf1_md:sha256", "-in", (work / "wk").string()},
                         &key, &err, 60000) == 0);
    REQUIRE(key.size() == 32);
    auto hex = [](const std::string &s) {
        static const char *k = "0123456789abcdef";
        std::string h;
        for (unsigned char c : s)
            h += {k[c >> 4], k[c & 15]};
        return h;
    };
    std::string plain;
    REQUIRE(Process::run({"openssl", "enc", "-d", "-aes-256-cbc", "-K", hex(key), "-iv", hex(b.substr(4 + n, 16)),
                          "-in", (work / "ct").string()},
                         &plain, &err, 60000) == 0);
    return plain;
}

} // namespace

TEST_CASE("uploader: the envelope opens with the private key") {
    const fs::path dir = fs::temp_directory_path() / ("babble_envelope_" + std::to_string(getpid()));
    fs::remove_all(dir);
    if (!make_keys(dir)) {
        MESSAGE("no openssl: skipped");
        return;
    }
    for (size_t size : {size_t(0), size_t(15), size_t(16), size_t(100000)}) {
        std::string data(size, '\0');
        for (size_t i = 0; i < size; i++)
            data[i] = char(i * 7 + 3);
        write(dir / "in", data);
        std::string err;
        REQUIRE_MESSAGE(Uploader::make_envelope(dir / "in", dir / "out", dir / "pub.pem", &err), err);
        CHECK_FALSE(fs::exists(dir / "out.part"));
        CHECK(open_envelope(dir / "out", dir / "priv.pem", dir) == data);
    }
    std::string err;
    CHECK_FALSE(Uploader::make_envelope(dir / "in", dir / "bad", dir / "missing.pem", &err));
    CHECK_FALSE(fs::exists(dir / "bad"));
    fs::remove_all(dir);
}

TEST_CASE("uploader: package, encrypt and PUT a consented session; resume after a failure") {
    const fs::path caps = fs::temp_directory_path() / ("babble_uploads_" + std::to_string(getpid()));
    const fs::path keys = caps / "keys";
    fs::remove_all(caps);
    if (!make_keys(keys)) {
        MESSAGE("no openssl: skipped");
        return;
    }
    auto make_session = [&](const std::string &id, bool consent) {
        json s = {{"format", 2}, {"id", id}, {"status", "complete"}, {"routine", "eye"},
                  {"consent", consent ? json{{"version", "t"}} : json(nullptr)}};
        write(caps / id / "session.json", s.dump());
    };
    make_session("s1", true);
    make_session("s2", false);

    std::mutex m;
    int packaged = 0, retry_packaging = 1;
    auto packager = [&](const fs::path &sdir, const PackagerOptions &o) {
        std::lock_guard<std::mutex> lock(m);
        PackageResult r;
        if (retry_packaging-- > 0) {   // the encoder was busy
            r.retry = true;
            r.error = "hardware encoder: busy";
            return r;
        }
        packaged++;
        CHECK(o.qp == 18);
        write(sdir / "package.tar", std::string(5000, 't'));
        r.ok = true;
        r.tar = sdir / "package.tar";
        r.package = {{"made_unix_ns", 1}};
        return r;
    };
    std::vector<Uploader::Http> puts;
    std::string received;
    int fail_puts = 1;   // the first PUT fails with a 503: retried without packaging again
    auto transport = [&](const Uploader::Http &r, const Uploader::Progress &progress) {
        std::lock_guard<std::mutex> lock(m);
        puts.push_back(r);
        Uploader::Response res;
        if (fail_puts-- > 0) {
            res.status = 503;
            return res;
        }
        if (progress)
            progress(1.0f);
        received = slurp(r.body_file);
        res.status = 200;
        return res;
    };
    const json cfg = {{"endpoint", "http://bucket.test:3900/"}, {"bucket", "babbledata"},  {"region", "garage"},
                      {"access_key", "AK"},                       {"secret_key", "S\"K"},     {"qp", 18},
                      {"public_key", slurp(keys / "pub.pem")}};
    Uploader none({{"endpoint", "http://x"}}, caps, json::object(), [] { return false; });
    CHECK_FALSE(none.available());   // no keys
    CHECK(none.queue("s1").code == "unavailable");

    Uploader up(cfg, caps, json::object(), [] { return false; }, transport, packager);
    CHECK(up.available());
    CHECK(up.queue("s2").code == "bad_request");   // no consent
    CHECK(up.queue("../etc").code == "bad_request");
    CHECK(up.queue("nope").code == "bad_request");
    REQUIRE(up.queue("s1").ok);
    up.start();
    auto state = [&] { return json::parse(slurp(caps / "s1" / "upload.json")); };
    // Each retry waits out a backoff (30 s, then 60): re-queueing clears it.
    // Wait for each attempt to end (active cleared after its backoff is set).
    REQUIRE(wait_for([&] {
        std::lock_guard<std::mutex> lock(m);
        return retry_packaging == 0 && up.snapshot().active.empty();
    }));
    REQUIRE(up.queue("s1").ok);
    REQUIRE(wait_for([&] {
        std::lock_guard<std::mutex> lock(m);
        return puts.size() == 1 && up.snapshot().active.empty();
    }));
    REQUIRE(up.queue("s1").ok);
    REQUIRE(wait_for([&] { return state().value("state", "") == "done"; }));

    std::lock_guard<std::mutex> lock(m);
    CHECK(packaged == 1);
    REQUIRE(puts.size() == 2);
    const json u = state();
    const std::string object = u["object"];
    CHECK(object.find("_frame-eye-v2_s1.tar.bin") != std::string::npos);
    CHECK(object.size() == 32 + 1 + 15 + std::string("_frame-eye-v2_s1.tar.bin").size());
    CHECK(puts[1].method == "PUT");
    CHECK(puts[1].url == "http://bucket.test:3900/babbledata/" + object);
    CHECK(puts[1].sigv4 == "aws:amz:garage:s3");
    CHECK(puts[1].credentials == "AK:S\"K");
    CHECK(puts[0].body_file == puts[1].body_file);   // the same object, encrypted once
    CHECK(received.size() == u["object_bytes"].get<uint64_t>());
    write(caps / "received.bin", received);
    CHECK(open_envelope(caps / "received.bin", keys / "priv.pem", caps) == std::string(5000, 't'));
    CHECK_FALSE(fs::exists(caps / "s1" / "package.tar.bin"));   // removed once sent
    CHECK(fs::exists(caps / "s1" / "package.tar"));
    CHECK_FALSE(fs::exists(caps / "s1" / ".upload-key.pem"));
    CHECK(up.queue("s1").code == "bad_request");   // already uploaded
    fs::remove_all(caps);
}
