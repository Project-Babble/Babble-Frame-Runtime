#include "camera/eye/scan/eye_ring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace babble::eyescan {

namespace {

using Profile = std::array<float, kStride>;

// The mean of each column (every 8th row), which all of the alignment uses.
Profile column_profile(const uint8_t *p) {
    Profile out{};
    for (size_t x = 0; x < kStride; x++) {
        uint32_t acc = 0;
        for (size_t y = 0; y < kHeight; y += 8)
            acc += p[y * kStride + x];
        out[x] = float(acc) / float(kHeight / 8);
    }
    return out;
}

double profile_distance(const Profile &a, const Profile &b, size_t rot) {
    double d = 0;
    for (size_t x = 0; x < kStride; x += 2)
        d += std::fabs(double(a[(x + rot) % kStride] - b[x]));
    return d / double(kStride / 2);
}

// How far `a` must rotate to align with `b`, and the distance that stays.
std::pair<size_t, double> best_rotation(const Profile &a, const Profile &b) {
    std::pair<size_t, double> best{0, std::numeric_limits<double>::infinity()};
    for (size_t r = 0; r < kStride; r++) {
        const double d = profile_distance(a, b, r);
        if (d < best.second)
            best = {r, d};
    }
    return best;
}

// The mean brightness of the 24 columns at each edge.
double edge_darkness(const uint8_t *p) {
    double acc = 0;
    size_t n = 0;
    for (size_t y = 0; y < kHeight; y += 4) {
        const uint8_t *r = p + y * kStride;
        for (size_t x = 0; x < 24; x++) {
            acc += double(r[x]) + double(r[kStride - 1 - x]);
            n += 2;
        }
    }
    return n ? acc / double(n) : 1e9;
}

// Where the picture near `hint` starts, aligned to a row: the first lit row after a run of dark rows.
std::optional<size_t> find_frame_start(const uint8_t *base, size_t len, size_t hint) {
    const int64_t hint_row = int64_t(hint / kStride);
    const int64_t max_row = int64_t(len / kStride) - 1;
    const int64_t lo = std::max<int64_t>(hint_row - 560, 0);
    const int64_t hi = std::min<int64_t>(hint_row + 560, max_row);
    if (hi - lo < 200)
        return std::nullopt;
    const size_t nrows = std::min<size_t>(size_t(hi - lo + 1), 1200);
    std::vector<double> mean(nrows);
    double sum = 0;
    for (size_t k = 0; k < nrows; k++) {
        const uint8_t *r = base + (size_t(lo) + k) * kStride;
        double acc = 0;
        for (size_t x = 0; x < kStride; x += 8)
            acc += r[x];
        mean[k] = acc / double(kStride / 8);
        sum += mean[k];
    }
    const double dark = std::max(sum / double(nrows) * 0.18, 1.5);
    std::optional<size_t> best;
    int64_t best_d = std::numeric_limits<int64_t>::max();
    int run = 0;
    for (size_t k = 0; k < nrows; k++) {
        if (mean[k] <= dark) {
            run++;
            continue;
        }
        if (run >= 8) {
            const int64_t row = lo + int64_t(k);
            const int64_t d = std::llabs(row - hint_row);
            if (d < best_d) {
                best_d = d;
                best = size_t(row) * kStride;
            }
        }
        run = 0;
    }
    return best;
}

std::vector<uint8_t> by_address(size_t n) {
    std::vector<uint8_t> g(n);
    for (size_t k = 0; k < n; k++)
        g[k] = k >= n / 2;
    return g;
}

std::string fmt_list(const std::vector<uint8_t> &v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? "," : "") + std::to_string(v[i]);
    return s + "]";
}

}

void frame_stats(const uint8_t *p, double *lit_out, double *rough_out) {
    size_t lit = 0, n = 0, nr = 0;
    double rough = 0;
    for (size_t y = 0; y < kHeight - 1; y += 4) {
        for (size_t x = 0; x < kStride - 1; x += 4) {
            const int v = p[y * kStride + x];
            n++;
            if (v > 16) {
                lit++;
                rough += std::abs(v - int(p[(y + 1) * kStride + x]));
                rough += std::abs(v - int(p[y * kStride + x + 1]));
                nr += 2;
            }
        }
    }
    *lit_out = n ? double(lit) / double(n) : 0.0;
    *rough_out = nr ? rough / double(nr) : 1e9;
}

bool looks_like_eye_frame(const uint8_t *p) {
    double lit, smooth;
    frame_stats(p, &lit, &smooth);
    return lit >= 0.20 && lit <= 0.96 && smooth <= 15.0;
}

// A byte is live when it is nonzero, and again when it changed. Pixels are almost never exactly 0 and change with
// each frame, but the row padding (columns 400..511) and the unused rest of the slot are zero and never change. So:
// first the column phase that leaves the least live 112-column band at columns 400..511 (ties: the most live
// picture), then the 400-row window with the most live rows (ties: the smallest move).
Refined refine_start(const uint8_t *region, size_t region_len, const uint16_t *changes) {
    const size_t rows = region_len / kStride;
    if (rows <= kHeight + 2 * kMarginRows)
        return {};
    auto live = [&](size_t i) -> uint32_t { return (region[i] != 0) + (changes ? changes[i] > 0 : 0); };
    std::array<uint32_t, kStride> col{};
    for (size_t r = 0; r < kHeight; r++) {
        const size_t row = (kMarginRows + r) * kStride;
        for (size_t x = 0; x < kStride; x++)
            col[x] += live(row + x);
    }
    auto band = [&](size_t d, size_t from, size_t to) {
        uint32_t s = 0;
        for (size_t j = from; j < to; j++)
            s += col[(d + j) % kStride];
        return s;
    };
    size_t d = 0;
    uint32_t best_pad = band(0, kWidth, kStride), best_pic = band(0, 0, kWidth);
    for (size_t c = 1; c < kStride; c++) {
        const uint32_t pad = band(c, kWidth, kStride), pic = band(c, 0, kWidth);
        if (pad < best_pad || (pad == best_pad && pic > best_pic)) {
            d = c;
            best_pad = pad;
            best_pic = pic;
        }
    }
    const size_t most = (kStride - kWidth) * kHeight * (changes ? 2 : 1);
    Refined out;
    out.pad_dirty = double(best_pad) / double(most);
    std::vector<uint64_t> act(rows - 1);
    for (size_t r = 0; r + 1 < rows; r++) {
        uint64_t s = 0;
        for (size_t x = 0; x < kWidth; x++)
            s += live(r * kStride + d + x);
        act[r] = s;
    }
    const int64_t margin = int64_t(kMarginRows);
    auto window = [&](int64_t m) {
        uint64_t s = 0;
        for (size_t i = 0; i < kHeight; i++)
            s += act[size_t(margin + m) + i];
        return s;
    };
    uint64_t busiest = 0;
    for (int64_t m = -margin; m <= margin; m++)
        busiest = std::max(busiest, window(m));
    int64_t pick = 0;
    bool have = false;
    for (int64_t m = -margin; m <= margin; m++) {
        if (double(window(m)) >= double(busiest) * 0.999 && (!have || std::llabs(m) < std::llabs(pick))) {
            pick = m;
            have = true;
        }
    }
    out.shift = int64_t(d) + pick * int64_t(kStride);
    return out;
}

std::vector<uint8_t> assign_groups(const std::vector<size_t> &off, size_t pitch, std::string *reason) {
    std::vector<size_t> gaps;
    for (size_t k = 1; k < off.size(); k++)
        gaps.push_back(off[k] - off[k - 1]);
    auto count = [&](size_t g) { return std::count(gaps.begin(), gaps.end(), g); };
    // The regular spacing is the one that most slots have (ties: the one nearest the measured pitch).
    if (!gaps.empty()) {
        size_t best = gaps[0];
        for (size_t g : gaps) {
            const auto cg = count(g), cb = count(best);
            const size_t dg = g > pitch ? g - pitch : pitch - g, db = best > pitch ? best - pitch : pitch - best;
            if (cg > cb || (cg == cb && dg <= db))
                best = g;
        }
        pitch = best;
    }
    std::vector<size_t> breaks;
    for (size_t k = 1; k < off.size(); k++)
        if (off[k] - off[k - 1] != pitch)
            breaks.push_back(k);
    size_t boundary;
    char buf[160];
    if (breaks.size() == 1) {
        const size_t b = breaks[0];
        const int64_t extra = int64_t(off[b]) - int64_t(off[b - 1]) - int64_t(pitch);
        snprintf(buf, sizeof(buf), "layout: group boundary before slot %zu (spacing %zu%+lld)", b, pitch,
                 (long long)extra);
        boundary = b;
    } else if (breaks.empty() && off.size() == 8) {
        snprintf(buf, sizeof(buf), "layout: no boundary in the spacing; 4 + 4 slots by address");
        boundary = 4;
    } else {
        return {};
    }
    if (reason)
        *reason = buf;
    std::vector<uint8_t> g(off.size());
    for (size_t k = 0; k < off.size(); k++)
        g[k] = k >= boundary;
    return g;
}

std::vector<uint8_t> groups_for(const std::vector<size_t> &off, size_t pitch, const std::vector<uint8_t> *similar,
                                bool *both, std::string *reason) {
    const bool full = off.size() == 2 * kSlotsPerCamera;
    std::string why;
    std::vector<uint8_t> g = assign_groups(off, pitch, &why);
    if (!g.empty()) {
        const size_t lower = size_t(std::count(g.begin(), g.end(), 0));
        if (full && lower != kSlotsPerCamera) {
            g = by_address(off.size());
            why = "4 + 4 slots by address (the " + why + " is not 4 + 4)";
        }
        *both = true;
    } else if (full && similar) {
        g = by_address(off.size());
        why = "4 + 4 slots by address (no group boundary in the layout)";
        *both = true;
    } else {
        g.assign(off.size(), 0);
        why = "one group only (one camera streaming)";
        *both = false;
    }
    if (similar && similar->size() == g.size()) {
        bool differ = false;
        for (size_t k = 0; k < g.size(); k++)
            differ |= ((*similar)[k] ^ (*similar)[0]) != (g[k] ^ g[0]);
        if (differ)
            why += "; WARNING: picture similarity groups the slots as " + fmt_list(*similar);
    }
    if (reason)
        *reason = why;
    return g;
}

bool each_group_full(const std::vector<uint8_t> &group) {
    for (uint8_t e : {0, 1})
        if (size_t(std::count(group.begin(), group.end(), e)) != kSlotsPerCamera)
            return false;
    return true;
}

bool discover(const DmaArena &arena, ScanClock &clock, const std::function<void(const std::string &)> &log,
              Look *look, Ring *out) {
    *look = Look{};
    char buf[256];
    const size_t len = arena.size();
    // ---- 1. Rough candidates: changed regions that look like pictures.
    std::vector<size_t> cand;
    const auto &changed = arena.block_changed();
    if (std::find(changed.begin(), changed.end(), true) != changed.end()) {
        const std::vector<uint8_t> snap = arena.snapshot();
        size_t off = 0;
        while (off + kFrameBytes <= len) {
            const size_t blk = off / DmaArena::kScanBlock;
            if (blk < changed.size() && changed[blk] && looks_like_eye_frame(snap.data() + off)) {
                const auto start = find_frame_start(snap.data(), len, off);
                if (start && *start + kFrameBytes <= len) {
                    bool dup = false;
                    for (size_t c : cand)
                        dup |= (c > *start ? c - *start : *start - c) < kFrameBytes / 2;
                    if (!dup && cand.size() < 32)
                        cand.push_back(*start);
                    off += kFrameBytes;
                    continue;
                }
            }
            off += 4096;
        }
    }
    look->candidates = cand.size();
    if (cand.size() < 2)
        return false;
    std::sort(cand.begin(), cand.end());
    snprintf(buf, sizeof(buf), "search: %zu candidate frame(s) in changed memory", cand.size());
    log(buf);

    // ---- 3. Keep only candidates that refill at least kMinRefreshes times in a second, not old pictures.
    std::vector<uint64_t> fp(cand.size());
    for (size_t k = 0; k < cand.size(); k++)
        fp[k] = arena.fingerprint(cand[k], kFrameBytes);
    std::vector<uint32_t> hits(cand.size(), 0);
    const double t0 = clock.now();
    while (clock.now() - t0 < 1.0 && !clock.stopped()) {
        for (size_t k = 0; k < cand.size(); k++) {
            const uint64_t f = arena.fingerprint(cand[k], kFrameBytes);
            if (f != fp[k]) {
                fp[k] = f;
                hits[k]++;
            }
        }
        clock.wait();
    }
    {
        std::string h = "refreshes in 1 s: [";
        for (size_t k = 0; k < hits.size(); k++)
            h += (k ? "," : "") + std::to_string(hits[k]);
        log(h + "]");
    }
    std::vector<uint32_t> sorted = hits;
    std::sort(sorted.begin(), sorted.end());
    look->refresh_hz = sorted[sorted.size() / 2] / std::max(clock.now() - t0, 1.0);
    look->stopped_at = "not_refreshing";
    {
        std::vector<size_t> keep;
        for (size_t k = 0; k < cand.size(); k++)
            if (hits[k] >= kMinRefreshes)
                keep.push_back(cand[k]);
        cand.swap(keep);
    }
    if (cand.size() < 2)
        return false;
    look->stopped_at = "few_slots";

    // ---- 4. Reference: the candidate with the darkest edges (the least likely to wrap around the frame edge).
    std::vector<uint8_t> snap = arena.snapshot();
    size_t base = cand[0];
    double base_dark = edge_darkness(snap.data() + base);
    for (size_t c : cand) {
        const double d = edge_darkness(snap.data() + c);
        if (d < base_dark) {
            base = c;
            base_dark = d;
        }
    }

    // ---- 5. Spacing. On a 256 KiB grid, a neighbour is rotated by how much the real spacing differs from 256 KiB.
    // Use the neighbour that aligns best, because that one holds the same camera.
    const Profile pref = column_profile(snap.data() + base);
    std::vector<std::pair<size_t, double>> options;
    if (base + kSlot + kFrameBytes <= len)
        options.push_back(best_rotation(column_profile(snap.data() + base + kSlot), pref));
    if (base >= kSlot)
        options.push_back(best_rotation(pref, column_profile(snap.data() + base - kSlot)));
    int64_t pitch = int64_t(kSlot);
    if (!options.empty()) {
        auto best = *std::min_element(options.begin(), options.end(),
                                      [](const auto &a, const auto &b) { return a.second < b.second; });
        const int64_t rot = int64_t(best.first);
        pitch = int64_t(kSlot) + (rot > int64_t(kStride / 2) ? rot - int64_t(kStride) : rot);
    }
    if (pitch != int64_t(kExpectedPitch)) {
        snprintf(buf, sizeof(buf), "note: measured slot spacing %lld, expected %zu", (long long)pitch, kExpectedPitch);
        log(buf);
    }

    // ---- 6. Lay the ring out on that spacing, while the frames look like pictures.
    const int64_t ilen = int64_t(len);
    int64_t first = int64_t(base);
    size_t walked = 0;
    while (first - pitch >= 0 && walked < kMaxRing && first - pitch + int64_t(kFrameBytes) <= ilen) {
        if (!looks_like_eye_frame(snap.data() + (first - pitch)))
            break;
        first -= pitch;
        walked++;
    }
    std::vector<size_t> off;
    for (int64_t o = first; o + int64_t(kFrameBytes + kStride) <= ilen && off.size() < kMaxRing; o += pitch) {
        if (!looks_like_eye_frame(snap.data() + o))
            break;
        off.push_back(size_t(o));
    }
    snap.clear();
    snap.shrink_to_fit();
    snprintf(buf, sizeof(buf), "ring walk: %zu slot(s) from 0x%llx, spacing %lld", off.size(),
             (unsigned long long)first, (long long)pitch);
    log(buf);
    if (off.size() < 2)
        return false;

    // ---- 7. Watch the slots for 0.6 s. Drop the slots that do not refresh (the walk can go one slot past the end of
    // the ring onto an old frame), and count how often each byte near each frame changes, for the framing.
    const size_t rows = kHeight + 2 * kMarginRows + 1;
    const size_t region = rows * kStride;
    std::vector<std::optional<size_t>> starts(off.size());
    for (size_t k = 0; k < off.size(); k++)
        if (off[k] >= kMarginRows * kStride && off[k] - kMarginRows * kStride + region <= len)
            starts[k] = off[k] - kMarginRows * kStride;
    std::vector<std::vector<uint8_t>> prev(off.size());
    std::vector<std::vector<uint16_t>> counts(off.size());
    std::vector<uint8_t> cur(region);
    for (size_t k = 0; k < off.size(); k++) {
        if (!starts[k])
            continue;
        prev[k].resize(region);
        counts[k].assign(region, 0);
        arena.copy(*starts[k], prev[k].data(), region);
    }
    std::vector<uint64_t> before(off.size());
    for (size_t k = 0; k < off.size(); k++)
        before[k] = arena.fingerprint(off[k], kFrameBytes);
    std::vector<bool> seen(off.size(), false);
    const double t1 = clock.now();
    double next_snap = t1 + 0.04;
    while (clock.now() - t1 < 0.6 && !clock.stopped()) {
        for (size_t k = 0; k < off.size(); k++)
            if (arena.fingerprint(off[k], kFrameBytes) != before[k])
                seen[k] = true;
        if (clock.now() >= next_snap) {
            next_snap += 0.04;
            for (size_t k = 0; k < off.size(); k++) {
                if (!starts[k])
                    continue;
                arena.copy(*starts[k], cur.data(), region);
                for (size_t i = 0; i < region; i++)
                    counts[k][i] += cur[i] != prev[k][i];
                prev[k].swap(cur);
            }
        }
        clock.wait();
    }
    if (std::count(seen.begin(), seen.end(), true) >= 2) {
        for (size_t k = off.size(); k-- > 0;) {
            if (!seen[k]) {
                snprintf(buf, sizeof(buf), "slot at 0x%zx did not refresh in 0.6 s, dropped", off[k]);
                log(buf);
                off.erase(off.begin() + long(k));
                prev.erase(prev.begin() + long(k));
                counts.erase(counts.begin() + long(k));
            }
        }
    }

    // ---- 8. Exact framing for each slot: the two groups are not on one grid, so each slot finds its own start.
    Ring ring;
    std::vector<size_t> exact;
    std::string shifts = "framing: start shifts [", dirty = "] dirty padding [";
    for (size_t k = 0; k < off.size(); k++) {
        if (prev[k].empty())
            continue;
        const Refined r = refine_start(prev[k].data(), prev[k].size(), counts[k].data());
        const int64_t start = int64_t(off[k]) + r.shift;
        if (start >= int64_t(kHeaderBytes) && start + int64_t(kFrameBytes) <= ilen) {
            exact.push_back(size_t(start));
            ring.shift.push_back(r.shift);
            ring.pad_dirty.push_back(r.pad_dirty);
            shifts += (exact.size() > 1 ? "," : "") + std::to_string(r.shift);
            snprintf(buf, sizeof(buf), "%s%.3f", exact.size() > 1 ? "," : "", r.pad_dirty);
            dirty += buf;
        }
    }
    log(shifts + dirty + "] (0 = clean)");
    if (exact.size() < 2)
        return false;

    // ---- 9. The group of each slot, from the memory layout. Picture similarity only checks it.
    std::vector<uint8_t> frame(kFrameBytes);
    std::vector<Profile> prof;
    for (size_t o : exact) {
        arena.copy(o, frame.data(), kFrameBytes);
        prof.push_back(column_profile(frame.data()));
    }
    std::vector<double> dist;
    double dmin = std::numeric_limits<double>::infinity(), dmax = -dmin;
    for (const auto &p : prof) {
        dist.push_back(profile_distance(p, prof[0], 0));
        dmin = std::min(dmin, dist.back());
        dmax = std::max(dmax, dist.back());
    }
    const double split = (dmin + dmax) / 2;
    std::vector<uint8_t> similar;
    for (double d : dist)
        similar.push_back(d > split);
    const bool pictures_differ = dmax - dmin >= 0.8;
    bool both = false;
    ring.group = groups_for(exact, size_t(pitch), pictures_differ ? &similar : nullptr, &both, &ring.reason);
    log("groups " + fmt_list(ring.group) + ": " + ring.reason);
    look->slots = exact.size();
    if (both && !each_group_full(ring.group)) {
        // Both cameras, but not all of their slots: a camera with fewer slots is read at a fraction of its rate.
        log("not taken: each camera needs 4 slots");
        return false;
    }
    look->both_groups = both;
    look->stopped_at = both ? "" : "one_eye";
    ring.pitch = size_t(pitch);
    ring.off = std::move(exact);
    ring.both_groups = both;
    *out = std::move(ring);
    return true;
}

}
