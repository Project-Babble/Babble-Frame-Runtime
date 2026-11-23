#pragma once

// Reads memory that a device writes without the CPU caches knowing.
//
// Ported from eyecam (tools/eyecam/src/mem.rs in https://github.com/sasaken1102r/frameeyeosc), which ports the
// approach from FrameEyeCameraFeed framestream.c

#include <cstddef>
#include <cstdint>
#include <vector>

namespace babble::eyescan {

// Drops the cache lines of [p, p + len) and waits until that is done. Does nothing on other architectures.
void invalidate(const uint8_t *p, size_t len);

// A read-only view of one shared buffer, plus the 64 KiB blocks that changed since the last clear.
class DmaArena {
public:
    static constexpr size_t kScanBlock = 65536;

    DmaArena() = default;
    // [ptr, ptr + len) must stay mapped and readable while the arena is in use.
    DmaArena(const uint8_t *ptr, size_t len);

    size_t size() const { return len_; }
    const uint8_t *data() const { return ptr_; }

    // A hash of 512 evenly spaced bytes of [off, off + len), read fresh from memory.
    uint64_t fingerprint(size_t off, size_t len) const;
    // Copies [off, off + len) to out, fresh from memory.
    void copy(size_t off, uint8_t *out, size_t len) const;
    // The full buffer, fresh from memory. Only the ring search uses this.
    std::vector<uint8_t> snapshot() const;

    // Marks each block whose fingerprint changed since the last call. Returns true if a block changed.
    bool note_block_changes();
    void clear_block_changes();
    // Takes the fingerprints again, so that only changes from now on count.
    void reset_block_changes();
    const std::vector<bool> &block_changed() const { return block_changed_; }

private:
    const uint8_t *ptr_ = nullptr;
    size_t len_ = 0;
    std::vector<uint64_t> block_fp_;
    std::vector<bool> block_changed_;
};

}
