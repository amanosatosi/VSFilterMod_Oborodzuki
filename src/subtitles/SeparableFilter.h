/*
    Copyright 2007  Niels Martin Hansen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    Contact:
    E-mail: <jiifurusu@gmail.com>
    IRC: jfs in #aegisub on irc.rizon.net

 */

#pragma once

#ifdef _OPENMP
#include <omp.h>
#endif
#include <math.h>
#include <algorithm>
#include <emmintrin.h>
#include <vector>

inline unsigned char NormalizeFilterValue(int sum, int divisor)
{
    const int value = sum / divisor;
    return static_cast<unsigned char>(value < 0 ? 0 : value > 255 ? 255 : value);
}

// Filter an image in horizontal direction with a one-dimensional filter
// PixelWidth is the distance in bytes between pixels
template<ptrdiff_t PixelDist>
void SeparableFilterX(unsigned char *src, unsigned char *dst, int width, int height, ptrdiff_t stride, int *kernel, int kernel_size, int divisor, int direction)
{
    const bool filterAxis = direction != 2;
    const int radius = kernel_size / 2;
    const int interiorEnd = width - (kernel_size - radius - 1);
    const ptrdiff_t tapStep = filterAxis ? PixelDist : 0;
#pragma omp parallel for
    for(int y = 0; y < height; y++)
    {
        const unsigned char *in = src + y * stride;
        unsigned char *out = dst + y * stride;
        for(int x = 0; x < width; x++)
        {
            int sum = 0;
            if (filterAxis && (x < radius || x >= interiorEnd))
            {
                for (int k = 0; k < kernel_size; ++k)
                {
                    int tap = x + k - radius;
                    if (tap < 0) tap += width;
                    if (tap >= width) tap -= width;
                    sum += in[tap * PixelDist] * kernel[k];
                }
            }
            else
            {
                const auto* tap = in + (x - (filterAxis ? radius : 0)) * PixelDist;
                for (int k = 0; k < kernel_size; ++k)
                    sum += tap[k * tapStep] * kernel[k];
            }
            out[x * PixelDist] = NormalizeFilterValue(sum, divisor);
        }
    }
}


// Filter an image in vertical direction with a one-dimensional filter
// Resolve wrapped source rows once per output row and traverse contiguous pixels.
// PixelDist selects the same interleaved channel in both passes.
template<ptrdiff_t PixelDist>
void SeparableFilterY(unsigned char *src, unsigned char *dst, int width, int height, ptrdiff_t stride, int *kernel, int kernel_size, int divisor, int direction)
{
    const bool filterAxis = direction != 1;
    const int radius = kernel_size / 2;
#pragma omp parallel
    {
        const unsigned char* localTaps[64];
        const unsigned char** taps = localTaps;
        std::vector<const unsigned char*> largeKernelTaps;
        if (kernel_size > 64)
        {
            largeKernelTaps.resize(kernel_size);
            taps = largeKernelTaps.data();
        }
#pragma omp for
        for(int y = 0; y < height; y++)
        {
            for (int k = 0; k < kernel_size; ++k)
            {
                int row = filterAxis ? y + k - radius : y;
                if (row < 0) row += height;
                if (row >= height) row -= height;
                taps[k] = src + row * stride;
            }
            auto* out = dst + y * stride;
            for (int x = 0; x < width; ++x)
            {
                int sum = 0;
                for (int k = 0; k < kernel_size; ++k)
                    sum += taps[k][x * PixelDist] * kernel[k];
                out[x * PixelDist] = NormalizeFilterValue(sum, divisor);
            }
        }
    }
}


static inline double NormalDist(double sigma, double x)
{
    if(sigma <= 0 && x == 0) return 1;
    else if(sigma <= 0) return 0;
    else return exp(-(x * x) / (2 * sigma * sigma)) / (sigma * sqrt(2 * 3.1415926535));
}


struct GaussianKernel
{
    int *kernel;
    int width;
    int divisor;
    inline GaussianKernel(double sigma)
    {
        width = (int)(sigma * 3 + 0.5) | 1; // binary-or with 1 to make sure the number is odd
        if(width < 3) width = 3;
        kernel = DNew int[width];
        kernel[width/2] = (int)(NormalDist(sigma, 0) * 255);
        divisor = kernel[width/2];
        for(int x = width / 2 - 1; x >= 0; x--)
        {
            int val = (int)(NormalDist(sigma, width / 2 - x) * 255 + 0.5);
            divisor += val * 2;
            kernel[x] = val;
            kernel[width - x - 1] = val;
        }
    }
    inline ~GaussianKernel()
    {
        delete[] kernel;
    }
};

// Filter eight neighbouring coverage bytes at once. Symmetric taps share one
// multiply. The caller checks that even an all-255 row fits a 16-bit sum.
inline void GaussianRow(const std::vector<const unsigned char*>& taps,
    const GaussianKernel& filter, unsigned char* result, int width)
{
    const auto zero = _mm_setzero_si128();
    const auto divisor = _mm_set1_epi16(short(filter.divisor));
    const auto reciprocal = _mm_set1_epi16(short(65536 / filter.divisor));
    const auto load = [&](const unsigned char* src) {
        return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(src)), zero);
    };
    const int mid = filter.width / 2;
    int x = 0;
    for (; x <= width - 8; x += 8) {
        auto sum = _mm_mullo_epi16(load(taps[mid] + x), _mm_set1_epi16(short(filter.kernel[mid])));
        for (int k = 0; k < mid; ++k) {
            const auto pair = _mm_add_epi16(load(taps[k] + x), load(taps[2 * mid - k] + x));
            sum = _mm_add_epi16(sum, _mm_mullo_epi16(pair, _mm_set1_epi16(short(filter.kernel[k]))));
        }
        // The truncated reciprocal underestimates by at most one. Its
        // remainder fits in a signed lane (less than twice the divisor).
        auto q = _mm_mulhi_epu16(sum, reciprocal);
        const auto remainder = _mm_sub_epi16(sum, _mm_mullo_epi16(q, divisor));
        q = _mm_sub_epi16(q, _mm_cmpgt_epi16(remainder, _mm_set1_epi16(short(filter.divisor - 1))));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(result + x), _mm_packus_epi16(q, zero));
    }
    for (; x < width; ++x) {
        int sum = 0;
        for (int k = 0; k < filter.width; ++k)
            sum += taps[k][x] * filter.kernel[k];
        result[x] = static_cast<unsigned char>(sum / filter.divisor);
    }
}

inline void GaussianCoverage(unsigned char* pixels, int width, int height, int channel,
    const GaussianKernel& filter, int direction)
{
    const int radius = filter.width / 2;
    const size_t pitch = size_t(width) * 2;
    std::vector<unsigned char> intermediate(size_t(width) * height);
    std::vector<unsigned char> row(size_t(width) + 2 * radius);
    std::vector<const unsigned char*> taps(filter.width);
    for (int k = 0; k < filter.width; ++k) taps[k] = row.data() + k;
    for (int y = 0; y < height; ++y) {
        const auto* src = pixels + y * pitch + channel;
        auto* dst = intermediate.data() + size_t(y) * width;
        auto* center = direction == 2 ? dst : row.data() + radius;
        for (int x = 0; x < width; ++x) center[x] = src[2 * x];
        if (direction != 2) {
            std::copy_n(center + width - radius, radius, row.data());
            std::copy_n(center, radius, center + width);
            GaussianRow(taps, filter, dst, width);
        }
    }
    for (int y = 0; y < height; ++y) {
        const auto* src = intermediate.data() + size_t(y) * width;
        if (direction != 1) {
            for (int k = 0; k < filter.width; ++k) {
                int iy = y + k - radius;
                if (iy < 0) iy += height;
                if (iy >= height) iy -= height;
                taps[k] = intermediate.data() + size_t(iy) * width;
            }
            GaussianRow(taps, filter, row.data(), width);
            src = row.data();
        }
        auto* dst = pixels + y * pitch + channel;
        for (int x = 0; x < width; ++x) dst[2 * x] = src[x];
    }
}
