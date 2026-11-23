#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <emmintrin.h>
#include <limits>
#include <utility>
#include <vector>

namespace RoundOutline {
using Interval = std::pair<int, int>;
struct Strip {
    int start;
    std::vector<short> left, right;
};

// Each strip contains one intersecting interval per consecutive scanline.
// Dilation of any contiguous slice is therefore exactly one interval.
inline Interval Bounds(const Strip& strip, int first, int last, const short* radius)
{
    int lo = INT16_MAX, hi = INT16_MIN;
    int i = first;
    if (last - first >= 8) {
        auto low = _mm_set1_epi16(INT16_MAX), high = _mm_set1_epi16(INT16_MIN);
        for (; i <= last - 8; i += 8) {
            const auto dx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(radius + i - first));
            const auto left = _mm_loadu_si128(reinterpret_cast<const __m128i*>(strip.left.data() + i));
            const auto right = _mm_loadu_si128(reinterpret_cast<const __m128i*>(strip.right.data() + i));
            low = _mm_min_epi16(low, _mm_sub_epi16(left, dx));
            high = _mm_max_epi16(high, _mm_add_epi16(right, dx));
        }
        low = _mm_min_epi16(low, _mm_srli_si128(low, 8));
        high = _mm_max_epi16(high, _mm_srli_si128(high, 8));
        low = _mm_min_epi16(low, _mm_srli_si128(low, 4));
        high = _mm_max_epi16(high, _mm_srli_si128(high, 4));
        low = _mm_min_epi16(low, _mm_srli_si128(low, 2));
        high = _mm_max_epi16(high, _mm_srli_si128(high, 2));
        lo = static_cast<short>(_mm_cvtsi128_si32(low));
        hi = static_cast<short>(_mm_cvtsi128_si32(high));
    }
    for (; i < last; ++i) {
        lo = (std::min)(lo, strip.left[i] - radius[i - first]);
        hi = (std::max)(hi, strip.right[i] + radius[i - first]);
    }
    return {lo, hi};
}

struct Rows {
    std::vector<unsigned char> safe;
    std::vector<unsigned> offsets;
    std::vector<Interval> intervals;
};

template<class RowTable, class Spans>
bool Build(const RowTable& rows, const Spans& spans, const std::vector<int>& radius, Rows& output)
{
    constexpr unsigned bias = 0x40000000;
    const int h = int(rows.size()) - 1, ry = int(radius.size()) - 1, rx = radius[0];
    if (ry < 3 || rx >= INT16_MAX || h > 16384 || spans.size() > 65536) return false;
    std::vector<short> kernel(2 * ry + 1);
    for (int i = -ry; i <= ry; ++i) kernel[ry + i] = short(radius[std::abs(i)]);
    output.safe.assign(h + 2 * ry, 1);
    std::vector<Strip> strips;
    std::vector<size_t> previous, current;
    for (int s = 0; s < h; ++s) {
        const size_t begin = rows[s].firstSpan, end = rows[s + 1].firstSpan;
        size_t p = s ? rows[s - 1].firstSpan : 0;
        size_t next = end;
        const size_t nextEnd = s + 1 < h ? rows[s + 2].firstSpan : end;
        current.clear();
        for (size_t j = begin; j < end; ++j) {
            const auto span = spans[j];
            if (span.first < bias || span.second - bias > unsigned(INT16_MAX - rx)) return false;
            const int l = int(span.first - bias), r = int(span.second - bias);
            while (p < begin && spans[p].second < span.first) ++p;
            size_t id;
            if (p < begin && spans[p].first <= span.second)
                id = previous[p++ - rows[s - 1].firstSpan];
            else {
                id = strips.size();
                strips.push_back({s, {}, {}});
                // Fragmented paths are cheaper to merge scanline by scanline.
                if (strips.size() * (2 * size_t(ry)) > 6 * spans.size() + 4096) return false;
            }
            strips[id].left.push_back(short(l));
            strips[id].right.push_back(short(r));
            current.push_back(id);

            // The historical merge may leave adjacent spans from the SAME
            // incoming row overlapping. A cluster is merged canonically when
            // its first span touches the existing accumulator. The preceding
            // source row (s+1, processed in descending order) is a sufficient
            // witness. Keep the historical path wherever that proof fails.
            if (j + 1 == end) continue;
            if (spans[j + 1].first < span.second) return false;
            const unsigned gap = spans[j + 1].first - span.second;
            if (gap > unsigned(2 * rx)) continue;
            while (next < nextEnd && spans[next].second < span.first) ++next;
            unsigned distance = UINT32_MAX;
            if (next < nextEnd)
                distance = spans[next].first > span.second ? spans[next].first - span.second : 0;
            if (next > end) distance = (std::min)(distance, unsigned(span.first - spans[next - 1].second));
            if (!gap) output.safe[s] = 0; // first source row at dy=+ry
            if (!distance) continue;
            for (int d = -ry; d <= ry; ++d) {
                const int dx = radius[std::abs(d)];
                const int prev = d == ry ? 0 : radius[std::abs(d + 1)];
                if (unsigned(2 * dx) >= gap && (d == ry || unsigned(dx + prev) < distance))
                    output.safe[s - d + ry] = 0;
            }
        }
        previous.swap(current);
    }
    if (std::count(output.safe.begin(), output.safe.end(), 1) < int(output.safe.size() / 2))
        return false;
    // Every strip contributes one interval to every certified row it
    // intersects. Count with its two endpoints before allocating the spans.
    std::vector<int> changes(output.safe.size() + 1);
    for (const auto& strip : strips) {
        ++changes[strip.start];
        --changes[strip.start + strip.left.size() + 2 * ry];
    }
    output.offsets.resize(changes.size());
    int active = 0;
    for (size_t i = 0; i < output.safe.size(); ++i) {
        active += changes[i];
        output.offsets[i + 1] = output.offsets[i] + (output.safe[i] ? active : 0);
    }
    output.intervals.resize(output.offsets.back());
    auto next = output.offsets;
    for (const auto& strip : strips) {
        const int n = int(strip.left.size());
        for (int y = -ry; y < n + ry; ++y) {
            const int row = strip.start + y + ry;
            if (!output.safe[row]) continue;
            const int first = (std::max)(0, y - ry), last = (std::min)(n, y + ry + 1);
            output.intervals[next[row]++] = Bounds(strip, first, last, kernel.data() + (ry + first - y));
        }
    }
    return true;
}
}
