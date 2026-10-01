#include "sink/osc.h"

#include <cstring>

namespace babble::osc {

namespace {

size_t padded(size_t n) { return (n + 4) & ~size_t(3); }   // string + NUL, rounded up to 4

} // namespace

size_t encode_float(std::string_view address, float value, uint8_t *out, size_t cap) {
    const size_t addr_len = padded(address.size());
    const size_t total = addr_len + 4 + 4;
    if (total > cap)
        return 0;
    memset(out, 0, total);
    memcpy(out, address.data(), address.size());
    memcpy(out + addr_len, ",f", 2);
    uint32_t bits;
    memcpy(&bits, &value, 4);
    bits = __builtin_bswap32(bits);
    memcpy(out + addr_len + 4, &bits, 4);
    return total;
}

void write_bundle_header(uint8_t *out) {
    memcpy(out, "#bundle\0", 8);
    memset(out + 8, 0, 8);
    out[15] = 1;   // time tag 1: immediately
}

void write_element_size(uint8_t *out, size_t n) {
    const uint32_t be = __builtin_bswap32(uint32_t(n));
    memcpy(out, &be, 4);
}

size_t encode_blob(std::string_view address, const uint8_t *data, size_t size, uint8_t *out, size_t cap) {
    const size_t addr_len = padded(address.size());
    const size_t data_len = (size + 3) & ~size_t(3);
    const size_t total = addr_len + 4 + 4 + data_len;
    if (total > cap || size > 0x7fffffff)
        return 0;
    memset(out, 0, total);
    memcpy(out, address.data(), address.size());
    memcpy(out + addr_len, ",b", 2);
    const uint32_t n = __builtin_bswap32(uint32_t(size));
    memcpy(out + addr_len + 4, &n, 4);
    if (size)
        memcpy(out + addr_len + 8, data, size);
    return total;
}

} // namespace babble::osc
