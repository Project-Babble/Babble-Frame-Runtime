#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace babble::osc {

// Encodes one OSC 1.0 message with a single float argument:
//   address (NUL-terminated, padded to 4) | ",f\0\0" | float32 big-endian
// Returns the encoded size, or 0 if it doesn't fit in `cap`.
size_t encode_float(std::string_view address, float value, uint8_t *out, size_t cap);

} // namespace babble::osc
