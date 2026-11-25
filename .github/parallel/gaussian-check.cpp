#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <random>
#include <vector>
#define DNew new
#include "../../src/subtitles/SeparableFilter.h"

int main()
{
    for (double sigma : {0.000001, 203.5, 204., 256., 1024.}) {
        GaussianKernel filter(sigma);
        long long sum = 0;
        for (int i = 0; i < filter.width; ++i) {
            if (filter.kernel[i] < 0 || filter.kernel[i] != filter.kernel[filter.width - i - 1])
                return 1;
            sum += filter.kernel[i];
        }
        if (sum != filter.divisor || sum <= 0 || sum * 255 > INT_MAX)
            return 1;
        if (sigma >= 204 && !filter.kernel[filter.width / 4])
            return 1;
        std::vector<unsigned char> row(filter.width, 173), out(row.size());
        SeparableFilterX<1>(row.data(), out.data(), filter.width, 1, filter.width,
            filter.kernel, filter.width, filter.divisor, 0);
        if (row != out) return 1;
    }
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
