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

} // namespace babble::osc
