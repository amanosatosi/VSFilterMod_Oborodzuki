#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <random>
#include <vector>
#define DNew new
#include "../../src/subtitles/SeparableFilter.h"

int main()
{
    std::mt19937 random(719);
    unsigned cases = 0;
    for (double sigma : {.42, .55, .8, 1., 1.5, 2., 3., 4., 6., 9., 20., 45.}) {
        GaussianKernel filter(sigma);
        if (filter.divisor <= 1 || filter.divisor > 257) continue;
        for (int extra = 0; extra < 17; ++extra) {
            const int w = filter.width + extra, h = filter.width + 3, pitch = 2 * w;
            for (int channel : {0, 1}) for (int axis : {0, 1, 2}) {
                std::vector<unsigned char> expected(h * pitch);
                for (auto& v : expected) v = static_cast<unsigned char>(random());
                auto actual = expected;
                std::vector<unsigned char> tmp(expected.size());
                SeparableFilterX<2>(expected.data() + channel, tmp.data(), w, h, pitch,
                    filter.kernel, filter.width, filter.divisor, axis);
                SeparableFilterY<2>(tmp.data(), expected.data() + channel, w, h, pitch,
                    filter.kernel, filter.width, filter.divisor, axis);
                GaussianCoverage(actual.data(), w, h, channel, filter, axis);
                if (actual != expected) {
                    std::fprintf(stderr, "Mismatch at sigma %g, width %d, channel %d, direction %d\n",
                        sigma, w, channel, axis);
                    return 1;
                }
                ++cases;
            }
        }
    }
    std::printf("%u Gaussian masks matched both scalar passes\n", cases);
    return cases ? 0 : 1;
}
