#include <cmath>
#include <cstdio>
#include <random>
#include "../../src/subtitles/RoundOutline.h"

using Row = std::vector<RoundOutline::Interval>;

// Model the legacy ordered two-list merge, including its unmerged tails.
// Raster coverage can count an overlapping tail twice.
static Row merge(const Row& accumulated, const Row& incoming)
{
    const Row* list[] = {&accumulated, &incoming};
    size_t cursor[] = {0, 0};
    Row out;
    while (cursor[0] < accumulated.size() && cursor[1] < incoming.size()) {
        int side = incoming[cursor[1]].first < accumulated[cursor[0]].first;
        auto span = (*list[side])[cursor[side]++];
        for (;;) {
            side ^= 1;
            auto& i = cursor[side];
            const auto& source = *list[side];
            if (i == source.size() || source[i].first > span.second) break;
            do {
                span.second = (std::max)(span.second, source[i++].second);
            } while (i < source.size() && source[i].first <= span.second);
        }
        out.push_back(span);
    }
    for (int side : {0, 1}) out.insert(out.end(), list[side]->begin() + cursor[side], list[side]->end());
    return out;
}

int main()
{
    std::mt19937 rng(41219);
    unsigned checked = 0, legacy = 0;
    for (int trial = 0; trial < 5000; ++trial) {
        const int h = 1 + rng() % 70, ry = 3 + rng() % 25, rx = rng() % 32;
        std::vector<int> radius(ry + 1);
        for (int d = 0; d <= ry; ++d)
            radius[d] = int(.5 + std::sqrt(float(ry * ry - d * d)) * float(rx) / float(ry));
        struct Source { size_t firstSpan; };
        std::vector<Source> rows;
        std::vector<std::pair<unsigned, unsigned>> spans;
        for (int s = 0; s < h; ++s) {
            rows.push_back({spans.size()});
            for (int x = rng() % 8; x < 110;) {
                int end = x + 1 + rng() % 19;
                if (rng() % 6)
                    spans.emplace_back(0x40000000u + x, 0x40000000u + end);
                x = end + rng() % 9;
            }
        }
        rows.push_back({spans.size()});
        RoundOutline::Rows result;
        if (!RoundOutline::Build(rows, spans, radius, result)) continue;
        for (int y = -ry; y < h + ry; ++y) {
            if (!result.safe[y + ry]) { ++legacy; continue; }
            Row expected;
            for (int s = (std::min)(h - 1, y + ry); s >= (std::max)(0, y - ry); --s) {
                Row incoming;
                for (size_t i = rows[s].firstSpan; i < rows[s + 1].firstSpan; ++i) {
                    const int dx = radius[std::abs(s - y)];
                    incoming.emplace_back(int(spans[i].first - 0x40000000) - dx,
                        int(spans[i].second - 0x40000000) + dx);
                }
                expected = merge(expected, incoming);
            }
            auto& intervals = result.intervals[y + ry];
            std::sort(intervals.begin(), intervals.end());
            Row actual;
            for (auto span : intervals) {
                if (actual.empty() || actual.back().second < span.first) actual.push_back(span);
                else actual.back().second = (std::max)(actual.back().second, span.second);
            }
            if (actual != expected) {
                std::fprintf(stderr, "Outline mismatch at trial %d, y %d\n", trial, y);
                return 1;
            }
            ++checked;
        }
    }
    std::printf("%u certified rows matched, %u ambiguous rows deferred\n", checked, legacy);
    return checked && legacy ? 0 : 1;
}
