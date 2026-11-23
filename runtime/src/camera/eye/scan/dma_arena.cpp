#include "camera/eye/scan/dma_arena.h"

#include <atomic>
#include <cassert>
#include <cstring>

namespace babble::eyescan {

namespace {

size_t cache_line() {
#if defined(__aarch64__)
    static const size_t line = [] {
        uint64_t ctr;
        asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
        return size_t(4) << ((ctr >> 16) & 0xf);   // DminLine is log2 of the line size in 4-byte words.
    }();
    return line;
#else
    return 64;
#endif
}

inline void drop_line(const uint8_t *p) {
#if defined(__aarch64__)
    asm volatile("dc civac, %0" : : "r"(p) : "memory");
#else
    (void)p;
#endif
}

inline void barrier() {
#if defined(__aarch64__)
    asm volatile("dsb ish" : : : "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

}

void invalidate(const uint8_t *p, size_t len) {
    const size_t line = cache_line();
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(line - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line)
        drop_line(reinterpret_cast<const uint8_t *>(a));
    barrier();
}

DmaArena::DmaArena(const uint8_t *ptr, size_t len)
    : ptr_(ptr), len_(len), block_fp_(len / kScanBlock, 0), block_changed_(len / kScanBlock, false) {
    reset_block_changes();
}

uint64_t DmaArena::fingerprint(size_t off, size_t len) const {
    assert(off + len <= len_);
    const size_t step = len / 512 > 0 ? len / 512 : 1;
    for (size_t i = 0; i < len; i += step)
        drop_line(ptr_ + off + i);
    barrier();
    uint64_t h = 1469598103934665603ull;   // idk man
    for (size_t i = 0; i < len; i += step) {
        h ^= *reinterpret_cast<const volatile uint8_t *>(ptr_ + off + i);
        h *= 1099511628211ull;
    }
    return h;
}

void DmaArena::copy(size_t off, uint8_t *out, size_t len) const {
    assert(off + len <= len_);
    invalidate(ptr_ + off, len);
    memcpy(out, ptr_ + off, len);
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

std::vector<uint8_t> DmaArena::snapshot() const {
    std::vector<uint8_t> out(len_);
    if (len_)
        copy(0, out.data(), len_);
    return out;
}

bool DmaArena::note_block_changes() {
    bool any = false;
    for (size_t b = 0; b < block_fp_.size(); b++) {
        const uint64_t h = fingerprint(b * kScanBlock, kScanBlock);
        if (h != block_fp_[b]) {
            block_fp_[b] = h;
            block_changed_[b] = true;
            any = true;
        }
    }
    return any;
}

void DmaArena::clear_block_changes() { block_changed_.assign(block_changed_.size(), false); }

void DmaArena::reset_block_changes() {
    note_block_changes();
    clear_block_changes();
}

}
