#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace babble::osc {

// Encodes one OSC 1.0 message with a single float argument:
//   address (NUL-terminated, padded to 4) | ",f\0\0" | float32 big-endian
// Returns the encoded size, or 0 if it doesn't fit in `cap`.
size_t encode_float(std::string_view address, float value, uint8_t *out, size_t cap);

// Encodes one OSC 1.0 message with a single blob argument:
//   address (padded to 4) | ",b\0\0" | int32 size big-endian | data (padded to 4)
// Returns the encoded size, or 0 if it doesn't fit in `cap`.
size_t encode_blob(std::string_view address, const uint8_t *data, size_t size, uint8_t *out, size_t cap);

// An OSC 1.0 bundle is "#bundle\0", a 64-bit time tag (1 = immediately), then
// elements, each an int32 big-endian size followed by a message.
constexpr size_t kBundleHeaderSize = 16;
void write_bundle_header(uint8_t *out);           // kBundleHeaderSize bytes
void write_element_size(uint8_t *out, size_t n);  // 4 bytes

} // namespace babble::osc
