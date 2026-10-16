#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace babble::osc {

// Encodes one OSC 1.0 message with one float argument and returns the size, or 0 if it does not fit in `cap`.
size_t encode_float(std::string_view address, float value, uint8_t *out, size_t cap);

// Encodes one OSC 1.0 message with one blob argument and returns the size, or 0 if it does not fit in `cap`.
size_t encode_blob(std::string_view address, const uint8_t *data, size_t size, uint8_t *out, size_t cap);

// An OSC 1.0 bundle has a 16-byte header, then elements that each have a big-endian int32 size and a message.
constexpr size_t kBundleHeaderSize = 16;
void write_bundle_header(uint8_t *out);           // This writes kBundleHeaderSize bytes.
void write_element_size(uint8_t *out, size_t n);  // This writes 4 bytes.

}
