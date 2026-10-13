#include <doctest/doctest.h>

#include "core/sha256.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace babble;

TEST_CASE("sha256: FIPS 180-4 vectors") {
    CHECK(Sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256: streamed in odd pieces, and of a file") {
    const std::string million(1000000, 'a');
    Sha256 h;
    for (size_t at = 0; at < million.size(); at += 977)
        h.update(million.data() + at, std::min<size_t>(977, million.size() - at));
    CHECK(h.finish_hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const auto path = std::filesystem::temp_directory_path() / ("babble_sha_" + std::to_string(getpid()));
    std::ofstream(path, std::ios::binary) << million;
    CHECK(Sha256::file_hex(path) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::filesystem::remove(path);
    CHECK_FALSE(Sha256::file_hex(path));
}
