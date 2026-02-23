/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/Common.h>
#include <tev/Task.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Demosaic.h>

#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

static Task<void> amazeDemosaic(
    const Vector2i size, span<const float> cfaIn, span<const uint8_t> cfaPattern, span<float> rgbOut, double initGain, int border, int priority
);

static Task<void> generalDemosaic(
    span<const float> cfaIn, span<float> rgbOut, const Vector2i size, span<const uint8_t> cfaPattern, const Vector2i cfaSize, int priority
);

Task<void> demosaic(
    span<const float> cfaIn, span<float> rgbOut, const Vector2i size, span<const uint8_t> cfaPattern, const Vector2i cfaSize, int priority
) {
    const auto isBayer = [&]() {
        if (cfaSize.y() != 2 || cfaSize.x() != 2) {
            return false;
        }

        int color_count[3] = {};
        for (int i = 0; i < 4; ++i) {
            if (cfaPattern[i] > 2) {
                return false;
            }
            color_count[cfaPattern[i]]++;
        }

        // Bayer must have exactly 1R, 2G, 1B
        if (!(color_count[0] == 1 && color_count[1] == 2 && color_count[2] == 1)) {
            return false;
        }

        return true;
    };

    // Use fancy demosaicing algorithm if we have a supported pattern, which generally gives better results than simple weighted interpolation.
    if (isBayer()) {
        co_await amazeDemosaic(
            size,
            cfaIn,
            cfaPattern,
            rgbOut,
            1.0, // initGain
            0, // border
            priority
        );
    } else {
        co_await generalDemosaic(cfaIn, rgbOut, size, cfaPattern, cfaSize, priority);
    }
}

static Task<void> generalDemosaic(
    span<const float> cfaIn, span<float> rgbOut, const Vector2i size, span<const uint8_t> cfaPattern, const Vector2i cfaSize, int priority
) {
    // The following is a vibe coded *general* demosaicing algorithm. Its quality is quite poor, but it lets us handle arbitrary CFA
    // patterns while still giving high-quality results for special-cased patterns above.

    // Build full CFA pattern lookup. CFA color indices per TIFF/EP:
    // 0=Red, 1=Green, 2=Blue, 3=Cyan, 4=Magenta, 5=Yellow, 6=White
    // We map these to output RGB channels. For standard R/G/B we use
    // direct mapping. Extended colors get a 3-channel contribution vector.

    // For each CFA color index, define its (r,g,b) contribution.
    // Standard Bayer: 0->R, 1->G, 2->B
    // Extended: approximate spectral response
    struct ColorWeight {
        float w[3]; // RGB contribution weights
    };

    const auto cfa_color_weight = [](uint8_t cfa_idx) -> ColorWeight {
        switch (cfa_idx) {
            case 0:
                return {
                    {1, 0, 0}
                }; // Red
            case 1:
                return {
                    {0, 1, 0}
                }; // Green
            case 2:
                return {
                    {0, 0, 1}
                }; // Blue
            case 3:
                return {
                    {0, 1, 1}
                }; // Cyan = G+B
            case 4:
                return {
                    {1, 0, 1}
                }; // Magenta = R+B
            case 5:
                return {
                    {1, 1, 0}
                }; // Yellow = R+G
            case 6:
                return {
                    {1, 1, 1}
                }; // White
            default:
                return {
                    {0, 0, 0}
                };
        }
    };

    // Determine the set of unique CFA color indices in the pattern
    // and how many output channels we need (always 3 for RGB output).
    const int w = size.x();
    const int h = size.y();

    // ============================================================
    // Generic demosaicing for arbitrary CFA patterns
    // ============================================================
    // Uses adaptive gradient-based interpolation generalized to any
    // repeat pattern. For each pixel and each missing output channel,
    // we find nearby CFA sites that contribute to that channel and
    // interpolate using inverse-distance weighting with edge-adaptive
    // gradient penalties.

    // Precompute: for each position in the repeat tile, and for each
    // output channel, the offsets to the nearest contributing sites
    // within a search radius.
    struct SampleOffset {
        int dx, dy;
        float baseWeight; // inverse distance weight
    };

    // For each tile position and each RGB channel, store the sample offsets.
    const int searchRadius = std::max(cfaSize.y(), cfaSize.x()) + 1;

    struct ChannelSamples {
        std::vector<SampleOffset> offsets;
    };

    // [tile_y][tile_x][channel]
    std::vector<std::vector<std::vector<ChannelSamples>>> tile_samples(
        cfaSize.y(), std::vector<std::vector<ChannelSamples>>(cfaSize.x(), std::vector<ChannelSamples>(3))
    );

    for (int ty = 0; ty < cfaSize.y(); ++ty) {
        for (int tx = 0; tx < cfaSize.x(); ++tx) {
            uint8_t my_cfa = cfaPattern[ty * cfaSize.x() + tx];
            ColorWeight my_w = cfa_color_weight(my_cfa);

            for (int ch = 0; ch < 3; ++ch) {
                auto& samples = tile_samples[ty][tx][ch];

                // Does this pixel's CFA color contribute to this channel?
                if (my_w.w[ch] > 0) {
                    // This pixel directly measures this channel
                    samples.offsets.push_back({0, 0, my_w.w[ch]});
                    continue;
                }

                // Find nearby CFA sites that contribute to this channel
                for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
                    for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }

                        int py = ((ty + dy) % cfaSize.y() + cfaSize.y()) % cfaSize.y();
                        int px = ((tx + dx) % cfaSize.x() + cfaSize.x()) % cfaSize.x();
                        uint8_t neighbor_cfa = cfaPattern[py * cfaSize.x() + px];
                        ColorWeight nw = cfa_color_weight(neighbor_cfa);

                        if (nw.w[ch] > 0) {
                            float dist = std::sqrt((float)(dx * dx + dy * dy));
                            float weight = nw.w[ch] / dist;
                            samples.offsets.push_back({dx, dy, weight});
                        }
                    }
                }

                // Keep only the closest ring of contributors to avoid
                // excessive blurring. Sort by distance and keep those
                // within 1.5x the minimum distance.
                if (!samples.offsets.empty()) {
                    float min_dist = std::numeric_limits<float>::max();
                    for (auto& s : samples.offsets) {
                        float d = std::sqrt((float)(s.dx * s.dx + s.dy * s.dy));
                        min_dist = std::min(min_dist, d);
                    }

                    float max_dist = min_dist * 1.6f;
                    std::erase_if(samples.offsets, [&](const SampleOffset& s) {
                        return std::sqrt((float)(s.dx * s.dx + s.dy * s.dy)) > max_dist;
                    });
                }
            }
        }
    }

    const auto numPixels = (size_t)size.x() * size.y();

    // Demosaic using edge-adaptive weighted interpolation.
    // At each pixel, for each missing channel, weight contributions
    // by both the precomputed base weight and an edge-sensitivity
    // term (penalize samples across strong gradients).
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        numPixels * 3,
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                const size_t idx = (size_t)y * w + x;

                int ty = (y % cfaSize.y() + cfaSize.y()) % cfaSize.y();
                int tx = (x % cfaSize.x() + cfaSize.x()) % cfaSize.x();

                float center = cfaIn[idx];

                for (int ch = 0; ch < 3; ++ch) {
                    const auto& samples = tile_samples[ty][tx][ch];

                    if (samples.offsets.size() == 1 && samples.offsets[0].dx == 0 && samples.offsets[0].dy == 0) {
                        // Direct measurement
                        rgbOut[idx * 3 + ch] = center * samples.offsets[0].baseWeight;
                        continue;
                    }

                    float weight_sum = 0;
                    float value_sum = 0;

                    // Epsilon to avoid division by zero
                    constexpr float eps = 1e-10f;

                    for (const auto& s : samples.offsets) {
                        int nx = clamp(x + s.dx, 0, w - 1);
                        int ny = clamp(y + s.dy, 0, h - 1);
                        float val = cfaIn[(size_t)ny * w + nx];

                        // Edge-adaptive weight: penalize if there's a
                        // large gradient between center and this sample.
                        // Use the CFA values along the path.
                        float gradient = std::abs(val - center);
                        float edge_weight = 1.0f / (gradient + eps);

                        float final_weight = s.baseWeight * edge_weight;
                        value_sum += val * final_weight;
                        weight_sum += final_weight;
                    }

                    rgbOut[idx * 3 + ch] = weight_sum > 0 ? value_sum / weight_sum : 0.0f;
                }
            }
        },
        priority
    );
}

static Task<void> amazeDemosaic(
    const Vector2i size, span<const float> cfaIn, span<const uint8_t> cfaPattern, span<float> rgbOut, double initGain, int border, int priority
) {
    // This is a vibe-coded port (i.e. derivative work) of librtprocess's AMaZE implementation to work within tev's thread pool.
    // https://github.com/CarVac/librtprocess/blob/master/src/demosaic/amaze.cc
    // Redistributed here under tev's GPLv3 license.

    // Validate Bayer CFA
    unsigned cfarray[2][2];
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            cfarray[r][c] = cfaPattern[r * 2 + c];
        }
    }

    const auto fc = [&](int row, int col) -> unsigned { return cfarray[row & 1][col & 1]; };

    {
        bool valid = true;
        int color_count[3] = {};
        for (int r = 0; r < 2 && valid; ++r) {
            for (int c = 0; c < 2 && valid; ++c) {
                if (cfarray[r][c] > 2) {
                    valid = false;
                }

                color_count[cfarray[r][c]]++;
            }
        }

        if (!valid || color_count[0] != 1 || color_count[1] != 2 || color_count[2] != 1) {
            throw std::runtime_error{"AMaZE: invalid Bayer CFA pattern"};
        }
    }

    // Helper lambdas to access cfaIn as row-major 2D and rgbOut as interleaved RGB
    const auto rawData = [&](int row, int col) -> float { return cfaIn[(size_t)row * size.x() + col]; };
    const auto setRed = [&](int row, int col, float val) { rgbOut[((size_t)row * size.x() + col) * 3 + 0] = val; };
    const auto setGreen = [&](int row, int col, float val) { rgbOut[((size_t)row * size.x() + col) * 3 + 1] = val; };
    const auto setBlue = [&](int row, int col, float val) { rgbOut[((size_t)row * size.x() + col) * 3 + 2] = val; };

    const float clipPt = 1.0f / (float)initGain;
    const float clipPt8 = 0.8f / (float)initGain;

    constexpr int ts = 160;
    constexpr int tsh = ts / 2;

    // Offset of R pixel within a Bayer quartet
    int ex, ey;
    if (fc(0, 0) == 1) {
        if (fc(0, 1) == 0) {
            ey = 0;
            ex = 1;
        } else {
            ey = 1;
            ex = 0;
        }
    } else {
        if (fc(0, 0) == 0) {
            ey = 0;
            ex = 0;
        } else {
            ey = 1;
            ex = 1;
        }
    }

    constexpr int v1 = ts, v2 = 2 * ts, v3 = 3 * ts;
    constexpr int p1 = -ts + 1, p2 = -2 * ts + 2, p3 = -3 * ts + 3;
    constexpr int m1 = ts + 1, m2 = 2 * ts + 2, m3 = 3 * ts + 3;

    constexpr float eps = 1e-5f, epssq = 1e-10f;
    constexpr float arthresh = 0.75f;

    constexpr float gaussodd[4] = {0.14659727707323927f, 0.103592713382435f, 0.0732036125103057f, 0.0365543548389495f};
    constexpr float nyqthresh = 0.5f;
    constexpr float gaussgrad[6] = {
        nyqthresh * 0.07384411893421103f,
        nyqthresh * 0.06207511968171489f,
        nyqthresh * 0.0521818194747806f,
        nyqthresh * 0.03687419286733595f,
        nyqthresh * 0.03099732204057846f,
        nyqthresh * 0.018413194161458882f
    };
    constexpr float gausseven[2] = {0.13719494435797422f, 0.05640252782101291f};
    constexpr float gquinc[4] = {0.169917f, 0.108947f, 0.069855f, 0.0287182f};

    const auto median3 = [](float a, float b, float c) -> float { return std::max(std::min(a, b), std::min(std::max(a, b), c)); };
    const auto intp = [](float wt, float a, float b) -> float { return wt * a + (1.0f - wt) * b; };
    const auto SQR = [](float x) -> float { return x * x; };

    // Build tile list
    struct Tile {
        int top, left;
    };

    std::vector<Tile> tiles;
    for (int top = -16; top < size.y(); top += ts - 32) {
        for (int left = -16; left < size.x(); left += ts - 32) {
            tiles.push_back({top, left});
        }
    }

    constexpr int cldf = 2;
    const size_t bufferSize = 14 * sizeof(float) * ts * ts + sizeof(unsigned char) * ts * tsh + 18 * cldf * 64 + 63;

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        (int)tiles.size(),
        (int)tiles.size() * ts * ts, // cost estimate
        [&](int tileIdx) {
            const int top = tiles[tileIdx].top;
            const int left = tiles[tileIdx].left;

            // Allocate working space
            std::vector<char> buffer(bufferSize, 0);

            float* data = (float*)((uintptr_t(buffer.data()) + uintptr_t(63)) / 64 * 64);

            float* rgbgreen = data;
            float* delhvsqsum = rgbgreen + ts * ts + cldf * 16;
            float* dirwts0 = delhvsqsum + ts * ts + cldf * 16;
            float* dirwts1 = dirwts0 + ts * ts + cldf * 16;
            float* vcd = dirwts1 + ts * ts + cldf * 16;
            float* hcd = vcd + ts * ts + cldf * 16;
            float* vcdalt = hcd + ts * ts + cldf * 16;
            float* hcdalt = vcdalt + ts * ts + cldf * 16;
            float* cddiffsq = hcdalt + ts * ts + cldf * 16;
            float* hvwt = cddiffsq + ts * ts + 2 * cldf * 16;

            float (*Dgrb)[ts * tsh] = (float (*)[ts * tsh]) vcdalt;
            float* delp = cddiffsq;
            float* delm = delp + ts * tsh + cldf * 16;
            float* rbint = delm;
            float* dgintv = hvwt + ts * tsh + cldf * 16;
            float* dginth = dgintv + ts * ts + cldf * 16;

            struct s_hv {
                float h;
                float v;
            };
            s_hv* Dgrb2 = (s_hv*)dgintv;

            float* Dgrbsq1m = dginth + ts * ts + cldf * 16;
            float* Dgrbsq1p = Dgrbsq1m + ts * tsh + cldf * 16;
            float* cfa = Dgrbsq1p + ts * tsh + cldf * 16;
            float* pmwt = delhvsqsum;
            float* rbm = vcd;
            float* rbp = rbm + ts * tsh + cldf * 16;

            unsigned char* nyquist = (unsigned char*)(cfa + ts * ts + cldf * 16);
            unsigned char* nyquist2 = (unsigned char*)cddiffsq;
            float* nyqutest = (float*)(nyquist + sizeof(unsigned char) * ts * tsh + cldf * 64);

            memset(&nyquist[3 * tsh], 0, sizeof(unsigned char) * (ts - 6) * tsh);

            int bottom = std::min(top + ts, size.y() + 16);
            int right_ = std::min(left + ts, size.x() + 16);
            int rr1 = bottom - top;
            int cc1 = right_ - left;

            int rrmin = top < 0 ? 16 : 0;
            int ccmin = left < 0 ? 16 : 0;
            int rrmax = bottom > size.y() ? size.y() - top : rr1;
            int ccmax = right_ > size.x() ? size.x() - left : cc1;

            // ===== Tile initialization =====

            // Fill upper border
            if (rrmin > 0) {
                for (int rr = 0; rr < 16; rr++) {
                    int row = 32 - rr + top;
                    for (int cc = ccmin; cc < ccmax; cc++) {
                        float temp = rawData(row, cc + left);
                        cfa[rr * ts + cc] = temp;
                        rgbgreen[rr * ts + cc] = temp;
                    }
                }
            }

            // Fill inner part
            for (int rr = rrmin; rr < rrmax; rr++) {
                int row = rr + top;
                for (int cc = ccmin; cc < ccmax; cc++) {
                    int indx1 = rr * ts + cc;
                    float temp = rawData(row, cc + left);
                    cfa[indx1] = temp;
                    rgbgreen[indx1] = temp;
                }
            }

            // Fill lower border
            if (rrmax < rr1) {
                for (int rr = 0; rr < 16; rr++) {
                    for (int cc = ccmin; cc < ccmax; cc++) {
                        float temp = rawData(size.y() - rr - 2, left + cc);
                        cfa[(rrmax + rr) * ts + cc] = temp;
                        rgbgreen[(rrmax + rr) * ts + cc] = temp;
                    }
                }
            }

            // Fill left border
            if (ccmin > 0) {
                for (int rr = rrmin; rr < rrmax; rr++) {
                    int row = rr + top;
                    for (int cc = 0; cc < 16; cc++) {
                        float temp = rawData(row, 32 - cc + left);
                        cfa[rr * ts + cc] = temp;
                        rgbgreen[rr * ts + cc] = temp;
                    }
                }
            }

            // Fill right border
            if (ccmax < cc1) {
                for (int rr = rrmin; rr < rrmax; rr++) {
                    for (int cc = 0; cc < 16; cc++) {
                        float temp = rawData(top + rr, size.x() - cc - 2);
                        cfa[rr * ts + ccmax + cc] = temp;
                        rgbgreen[rr * ts + ccmax + cc] = temp;
                    }
                }
            }

            // Fill corners
            if (rrmin > 0 && ccmin > 0) {
                for (int rr = 0; rr < 16; rr++) {
                    for (int cc = 0; cc < 16; cc++) {
                        float temp = rawData(32 - rr, 32 - cc);
                        cfa[rr * ts + cc] = temp;
                        rgbgreen[rr * ts + cc] = temp;
                    }
                }
            }

            if (rrmax < rr1 && ccmax < cc1) {
                for (int rr = 0; rr < 16; rr++) {
                    for (int cc = 0; cc < 16; cc++) {
                        float temp = rawData(size.y() - rr - 2, size.x() - cc - 2);
                        cfa[(rrmax + rr) * ts + ccmax + cc] = temp;
                        rgbgreen[(rrmax + rr) * ts + ccmax + cc] = temp;
                    }
                }
            }

            if (rrmin > 0 && ccmax < cc1) {
                for (int rr = 0; rr < 16; rr++) {
                    for (int cc = 0; cc < 16; cc++) {
                        float temp = rawData(32 - rr, size.x() - cc - 2);
                        cfa[rr * ts + ccmax + cc] = temp;
                        rgbgreen[rr * ts + ccmax + cc] = temp;
                    }
                }
            }

            if (rrmax < rr1 && ccmin > 0) {
                for (int rr = 0; rr < 16; rr++) {
                    for (int cc = 0; cc < 16; cc++) {
                        float temp = rawData(size.y() - rr - 2, 32 - cc);
                        cfa[(rrmax + rr) * ts + cc] = temp;
                        rgbgreen[(rrmax + rr) * ts + cc] = temp;
                    }
                }
            }

            // ===== Horizontal and vertical gradients =====
            for (int rr = 2; rr < rr1 - 2; rr++) {
                for (int cc = 2, indx = rr * ts + cc; cc < cc1 - 2; cc++, indx++) {
                    float delh = fabsf(cfa[indx + 1] - cfa[indx - 1]);
                    float delv = fabsf(cfa[indx + v1] - cfa[indx - v1]);
                    dirwts0[indx] = eps + fabsf(cfa[indx + v2] - cfa[indx]) + fabsf(cfa[indx] - cfa[indx - v2]) + delv;
                    dirwts1[indx] = eps + fabsf(cfa[indx + 2] - cfa[indx]) + fabsf(cfa[indx] - cfa[indx - 2]) + delh;
                    delhvsqsum[indx] = SQR(delh) + SQR(delv);
                }
            }

            // ===== Interpolate vertical and horizontal colour differences =====
            for (int rr = 4; rr < rr1 - 4; rr++) {
                bool fcswitch = fc(rr, 4) & 1;

                for (int cc = 4, indx = rr * ts + cc; cc < cc1 - 4; cc++, indx++) {
                    float cru = cfa[indx - v1] * (dirwts0[indx - v2] + dirwts0[indx]) /
                        (dirwts0[indx - v2] * (eps + cfa[indx]) + dirwts0[indx] * (eps + cfa[indx - v2]));
                    float crd = cfa[indx + v1] * (dirwts0[indx + v2] + dirwts0[indx]) /
                        (dirwts0[indx + v2] * (eps + cfa[indx]) + dirwts0[indx] * (eps + cfa[indx + v2]));
                    float crl = cfa[indx - 1] * (dirwts1[indx - 2] + dirwts1[indx]) /
                        (dirwts1[indx - 2] * (eps + cfa[indx]) + dirwts1[indx] * (eps + cfa[indx - 2]));
                    float crr = cfa[indx + 1] * (dirwts1[indx + 2] + dirwts1[indx]) /
                        (dirwts1[indx + 2] * (eps + cfa[indx]) + dirwts1[indx] * (eps + cfa[indx + 2]));

                    float guha = cfa[indx - v1] + 0.5f * (cfa[indx] - cfa[indx - v2]);
                    float gdha = cfa[indx + v1] + 0.5f * (cfa[indx] - cfa[indx + v2]);
                    float glha = cfa[indx - 1] + 0.5f * (cfa[indx] - cfa[indx - 2]);
                    float grha = cfa[indx + 1] + 0.5f * (cfa[indx] - cfa[indx + 2]);

                    float guar = fabsf(1.f - cru) < arthresh ? cfa[indx] * cru : guha;
                    float gdar = fabsf(1.f - crd) < arthresh ? cfa[indx] * crd : gdha;
                    float glar = fabsf(1.f - crl) < arthresh ? cfa[indx] * crl : glha;
                    float grar = fabsf(1.f - crr) < arthresh ? cfa[indx] * crr : grha;

                    float hwt = dirwts1[indx - 1] / (dirwts1[indx - 1] + dirwts1[indx + 1]);
                    float vwt = dirwts0[indx - v1] / (dirwts0[indx + v1] + dirwts0[indx - v1]);

                    float Gintvha = vwt * gdha + (1.f - vwt) * guha;
                    float Ginthha = hwt * grha + (1.f - hwt) * glha;

                    if (fcswitch) {
                        vcd[indx] = cfa[indx] - (vwt * gdar + (1.f - vwt) * guar);
                        hcd[indx] = cfa[indx] - (hwt * grar + (1.f - hwt) * glar);
                        vcdalt[indx] = cfa[indx] - Gintvha;
                        hcdalt[indx] = cfa[indx] - Ginthha;
                    } else {
                        vcd[indx] = (vwt * gdar + (1.f - vwt) * guar) - cfa[indx];
                        hcd[indx] = (hwt * grar + (1.f - hwt) * glar) - cfa[indx];
                        vcdalt[indx] = Gintvha - cfa[indx];
                        hcdalt[indx] = Ginthha - cfa[indx];
                    }

                    fcswitch = !fcswitch;

                    if (cfa[indx] > clipPt8 || Gintvha > clipPt8 || Ginthha > clipPt8) {
                        guar = guha;
                        gdar = gdha;
                        glar = glha;
                        grar = grha;
                        vcd[indx] = vcdalt[indx];
                        hcd[indx] = hcdalt[indx];
                    }

                    dgintv[indx] = std::min(SQR(guha - gdha), SQR(guar - gdar));
                    dginth[indx] = std::min(SQR(glha - grha), SQR(glar - grar));
                }
            }

            // ===== Variance-based smoothing of colour differences =====
            for (int rr = 4; rr < rr1 - 4; rr++) {
                for (int cc = 4, indx = rr * ts + cc, c = fc(rr, cc) & 1; cc < cc1 - 4; cc++, indx++) {
                    float hcdvar = 3.f * (SQR(hcd[indx - 2]) + SQR(hcd[indx]) + SQR(hcd[indx + 2])) -
                        SQR(hcd[indx - 2] + hcd[indx] + hcd[indx + 2]);
                    float hcdaltvar = 3.f * (SQR(hcdalt[indx - 2]) + SQR(hcdalt[indx]) + SQR(hcdalt[indx + 2])) -
                        SQR(hcdalt[indx - 2] + hcdalt[indx] + hcdalt[indx + 2]);
                    float vcdvar = 3.f * (SQR(vcd[indx - v2]) + SQR(vcd[indx]) + SQR(vcd[indx + v2])) -
                        SQR(vcd[indx - v2] + vcd[indx] + vcd[indx + v2]);
                    float vcdaltvar = 3.f * (SQR(vcdalt[indx - v2]) + SQR(vcdalt[indx]) + SQR(vcdalt[indx + v2])) -
                        SQR(vcdalt[indx - v2] + vcdalt[indx] + vcdalt[indx + v2]);

                    if (hcdaltvar < hcdvar) {
                        hcd[indx] = hcdalt[indx];
                    }
                    if (vcdaltvar < vcdvar) {
                        vcd[indx] = vcdalt[indx];
                    }

                    float Gintv, Ginth;

                    if (c) {
                        Ginth = -hcd[indx] + cfa[indx];
                        Gintv = -vcd[indx] + cfa[indx];

                        if (hcd[indx] > 0) {
                            if (3.f * hcd[indx] > (Ginth + cfa[indx])) {
                                hcd[indx] = -median3(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx];
                            } else {
                                float hwt2 = 1.f - 3.f * hcd[indx] / (eps + Ginth + cfa[indx]);
                                hcd[indx] = hwt2 * hcd[indx] + (1.f - hwt2) * (-median3(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx]);
                            }
                        }

                        if (vcd[indx] > 0) {
                            if (3.f * vcd[indx] > (Gintv + cfa[indx])) {
                                vcd[indx] = -median3(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx];
                            } else {
                                float vwt2 = 1.f - 3.f * vcd[indx] / (eps + Gintv + cfa[indx]);
                                vcd[indx] = vwt2 * vcd[indx] + (1.f - vwt2) * (-median3(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx]);
                            }
                        }

                        if (Ginth > clipPt) {
                            hcd[indx] = -median3(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx];
                        }
                        if (Gintv > clipPt) {
                            vcd[indx] = -median3(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx];
                        }

                    } else {
                        Ginth = hcd[indx] + cfa[indx];
                        Gintv = vcd[indx] + cfa[indx];

                        if (hcd[indx] < 0) {
                            if (3.f * hcd[indx] < -(Ginth + cfa[indx])) {
                                hcd[indx] = median3(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx];
                            } else {
                                float hwt2 = 1.f + 3.f * hcd[indx] / (eps + Ginth + cfa[indx]);
                                hcd[indx] = hwt2 * hcd[indx] + (1.f - hwt2) * (median3(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx]);
                            }
                        }

                        if (vcd[indx] < 0) {
                            if (3.f * vcd[indx] < -(Gintv + cfa[indx])) {
                                vcd[indx] = median3(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx];
                            } else {
                                float vwt2 = 1.f + 3.f * vcd[indx] / (eps + Gintv + cfa[indx]);
                                vcd[indx] = vwt2 * vcd[indx] + (1.f - vwt2) * (median3(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx]);
                            }
                        }

                        if (Ginth > clipPt) {
                            hcd[indx] = median3(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx];
                        }
                        if (Gintv > clipPt) {
                            vcd[indx] = median3(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx];
                        }

                        cddiffsq[indx] = SQR(vcd[indx] - hcd[indx]);
                    }

                    c = !c;
                }
            }

            // ===== Adaptive weights for G interpolation =====
            for (int rr = 6; rr < rr1 - 6; rr++) {
                for (int cc = 6 + (fc(rr, 2) & 1), indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2) {
                    float uave = vcd[indx] + vcd[indx - v1] + vcd[indx - v2] + vcd[indx - v3];
                    float dave = vcd[indx] + vcd[indx + v1] + vcd[indx + v2] + vcd[indx + v3];
                    float lave = hcd[indx] + hcd[indx - 1] + hcd[indx - 2] + hcd[indx - 3];
                    float rave = hcd[indx] + hcd[indx + 1] + hcd[indx + 2] + hcd[indx + 3];

                    float Dgrbvvaru = SQR(vcd[indx] - uave) + SQR(vcd[indx - v1] - uave) + SQR(vcd[indx - v2] - uave) +
                        SQR(vcd[indx - v3] - uave);
                    float Dgrbvvard = SQR(vcd[indx] - dave) + SQR(vcd[indx + v1] - dave) + SQR(vcd[indx + v2] - dave) +
                        SQR(vcd[indx + v3] - dave);
                    float Dgrbhvarl = SQR(hcd[indx] - lave) + SQR(hcd[indx - 1] - lave) + SQR(hcd[indx - 2] - lave) +
                        SQR(hcd[indx - 3] - lave);
                    float Dgrbhvarr = SQR(hcd[indx] - rave) + SQR(hcd[indx + 1] - rave) + SQR(hcd[indx + 2] - rave) +
                        SQR(hcd[indx + 3] - rave);

                    float hwt = dirwts1[indx - 1] / (dirwts1[indx - 1] + dirwts1[indx + 1]);
                    float vwt = dirwts0[indx - v1] / (dirwts0[indx + v1] + dirwts0[indx - v1]);

                    float vcdvar = epssq + vwt * Dgrbvvard + (1.f - vwt) * Dgrbvvaru;
                    float hcdvar = epssq + hwt * Dgrbhvarr + (1.f - hwt) * Dgrbhvarl;

                    float Dgrbvvaru2 = dgintv[indx] + dgintv[indx - v1] + dgintv[indx - v2];
                    float Dgrbvvard2 = dgintv[indx] + dgintv[indx + v1] + dgintv[indx + v2];
                    float Dgrbhvarl2 = dginth[indx] + dginth[indx - 1] + dginth[indx - 2];
                    float Dgrbhvarr2 = dginth[indx] + dginth[indx + 1] + dginth[indx + 2];

                    float vcdvar1 = epssq + vwt * Dgrbvvard2 + (1.f - vwt) * Dgrbvvaru2;
                    float hcdvar1 = epssq + hwt * Dgrbhvarr2 + (1.f - hwt) * Dgrbhvarl2;

                    float varwt = hcdvar / (vcdvar + hcdvar);
                    float diffwt = hcdvar1 / (vcdvar1 + hcdvar1);

                    if ((0.5f - varwt) * (0.5f - diffwt) > 0 && fabsf(0.5f - diffwt) < fabsf(0.5f - varwt)) {
                        hvwt[indx >> 1] = varwt;
                    } else {
                        hvwt[indx >> 1] = diffwt;
                    }
                }
            }

            // ===== Nyquist test =====
            for (int rr = 6; rr < rr1 - 6; rr++) {
                for (int cc = 6 + (fc(rr, 2) & 1), indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2) {
                    nyqutest[indx >> 1] =
                        (gaussodd[0] * cddiffsq[indx] +
                         gaussodd[1] * (cddiffsq[indx - m1] + cddiffsq[indx + p1] + cddiffsq[indx - p1] + cddiffsq[indx + m1]) +
                         gaussodd[2] * (cddiffsq[indx - v2] + cddiffsq[indx - 2] + cddiffsq[indx + 2] + cddiffsq[indx + v2]) +
                         gaussodd[3] * (cddiffsq[indx - m2] + cddiffsq[indx + p2] + cddiffsq[indx - p2] + cddiffsq[indx + m2])) -
                        (gaussgrad[0] * delhvsqsum[indx] +
                         gaussgrad[1] * (delhvsqsum[indx - v1] + delhvsqsum[indx + 1] + delhvsqsum[indx - 1] + delhvsqsum[indx + v1]) +
                         gaussgrad[2] * (delhvsqsum[indx - m1] + delhvsqsum[indx + p1] + delhvsqsum[indx - p1] + delhvsqsum[indx + m1]) +
                         gaussgrad[3] * (delhvsqsum[indx - v2] + delhvsqsum[indx - 2] + delhvsqsum[indx + 2] + delhvsqsum[indx + v2]) +
                         gaussgrad[4] *
                             (delhvsqsum[indx - v2 - 1] + delhvsqsum[indx - v2 + 1] + delhvsqsum[indx - ts - 2] + delhvsqsum[indx - ts + 2] +
                              delhvsqsum[indx + ts - 2] + delhvsqsum[indx + ts + 2] + delhvsqsum[indx + v2 - 1] + delhvsqsum[indx + v2 + 1]) +
                         gaussgrad[5] * (delhvsqsum[indx - m2] + delhvsqsum[indx + p2] + delhvsqsum[indx - p2] + delhvsqsum[indx + m2]));
                }
            }

            int nystartrow = 0, nyendrow = 0;
            int nystartcol = ts + 1, nyendcol = 0;

            for (int rr = 6; rr < rr1 - 6; rr++) {
                for (int cc = 6 + (fc(rr, 2) & 1), indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2) {
                    if (nyqutest[indx >> 1] > 0.f) {
                        nyquist[indx >> 1] = 1;
                        nystartrow = nystartrow ? nystartrow : rr;
                        nyendrow = rr;
                        nystartcol = nystartcol > cc ? cc : nystartcol;
                        nyendcol = nyendcol < cc ? cc : nyendcol;
                    }
                }
            }

            bool doNyquist = nystartrow != nyendrow && nystartcol != nyendcol;

            if (doNyquist) {
                nyendrow++;
                nyendcol++;
                nystartcol -= (nystartcol & 1);
                nystartrow = std::max(8, nystartrow);
                nyendrow = std::min(rr1 - 8, nyendrow);
                nystartcol = std::max(8, nystartcol);
                nyendcol = std::min(cc1 - 8, nyendcol);
                memset(&nyquist2[4 * tsh], 0, sizeof(char) * (ts - 8) * tsh);

                for (int rr = nystartrow; rr < nyendrow; rr++) {
                    for (int indx = rr * ts + nystartcol + (fc(rr, 2) & 1); indx < rr * ts + nyendcol; indx += 2) {
                        unsigned int nyquisttemp = nyquist[(indx - v2) >> 1] + nyquist[(indx - m1) >> 1] + nyquist[(indx + p1) >> 1] +
                            nyquist[(indx - 2) >> 1] + nyquist[(indx + 2) >> 1] + nyquist[(indx - p1) >> 1] + nyquist[(indx + m1) >> 1] +
                            nyquist[(indx + v2) >> 1];
                        nyquist2[indx >> 1] = nyquisttemp > 4 ? 1 : (nyquisttemp < 4 ? 0 : nyquist[indx >> 1]);
                    }
                }

                // Area interpolation in Nyquist regions
                for (int rr = nystartrow; rr < nyendrow; rr++) {
                    for (int indx = rr * ts + nystartcol + (fc(rr, 2) & 1); indx < rr * ts + nyendcol; indx += 2) {
                        if (nyquist2[indx >> 1]) {
                            float sumcfa = 0.f, sumh = 0.f, sumv = 0.f, sumsqh = 0.f, sumsqv = 0.f, areawt = 0.f;

                            for (int i = -6; i < 7; i += 2) {
                                int indx1 = indx + (i * ts) - 6;
                                for (int j = -6; j < 7; j += 2, indx1 += 2) {
                                    if (nyquist2[indx1 >> 1]) {
                                        float cfatemp = cfa[indx1];
                                        sumcfa += cfatemp;
                                        sumh += (cfa[indx1 - 1] + cfa[indx1 + 1]);
                                        sumv += (cfa[indx1 - v1] + cfa[indx1 + v1]);
                                        sumsqh += SQR(cfatemp - cfa[indx1 - 1]) + SQR(cfatemp - cfa[indx1 + 1]);
                                        sumsqv += SQR(cfatemp - cfa[indx1 - v1]) + SQR(cfatemp - cfa[indx1 + v1]);
                                        areawt += 1;
                                    }
                                }
                            }

                            sumh = sumcfa - 0.5f * sumh;
                            sumv = sumcfa - 0.5f * sumv;
                            areawt = 0.5f * areawt;
                            float hcdvar2 = epssq + fabsf(areawt * sumsqh - sumh * sumh);
                            float vcdvar2 = epssq + fabsf(areawt * sumsqv - sumv * sumv);
                            hvwt[indx >> 1] = hcdvar2 / (vcdvar2 + hcdvar2);
                        }
                    }
                }
            }

            // ===== Populate G at R/B sites =====
            for (int rr = 8; rr < rr1 - 8; rr++) {
                for (int indx = rr * ts + 8 + (fc(rr, 2) & 1); indx < rr * ts + cc1 - 8; indx += 2) {
                    float hvwtalt = 0.25f *
                        (hvwt[(indx - m1) >> 1] + hvwt[(indx + p1) >> 1] + hvwt[(indx - p1) >> 1] + hvwt[(indx + m1) >> 1]);

                    hvwt[indx >> 1] = fabsf(0.5f - hvwt[indx >> 1]) < fabsf(0.5f - hvwtalt) ? hvwtalt : hvwt[indx >> 1];

                    Dgrb[0][indx >> 1] = intp(hvwt[indx >> 1], vcd[indx], hcd[indx]);
                    rgbgreen[indx] = cfa[indx] + Dgrb[0][indx >> 1];

                    Dgrb2[indx >> 1].h = nyquist2[indx >> 1] ? SQR(rgbgreen[indx] - 0.5f * (rgbgreen[indx - 1] + rgbgreen[indx + 1])) : 0.f;
                    Dgrb2[indx >> 1].v = nyquist2[indx >> 1] ? SQR(rgbgreen[indx] - 0.5f * (rgbgreen[indx - v1] + rgbgreen[indx + v1])) : 0.f;
                }
            }

            // ===== Refine Nyquist areas using G curvatures =====
            if (doNyquist) {
                for (int rr = nystartrow; rr < nyendrow; rr++) {
                    for (int indx = rr * ts + nystartcol + (fc(rr, 2) & 1); indx < rr * ts + nyendcol; indx += 2) {
                        if (nyquist2[indx >> 1]) {
                            float gvarh = epssq + gquinc[0] * Dgrb2[indx >> 1].h +
                                gquinc[1] *
                                    (Dgrb2[(indx - m1) >> 1].h + Dgrb2[(indx + p1) >> 1].h + Dgrb2[(indx - p1) >> 1].h +
                                     Dgrb2[(indx + m1) >> 1].h) +
                                gquinc[2] *
                                    (Dgrb2[(indx - v2) >> 1].h + Dgrb2[(indx - 2) >> 1].h + Dgrb2[(indx + 2) >> 1].h +
                                     Dgrb2[(indx + v2) >> 1].h) +
                                gquinc[3] *
                                    (Dgrb2[(indx - m2) >> 1].h + Dgrb2[(indx + p2) >> 1].h + Dgrb2[(indx - p2) >> 1].h +
                                     Dgrb2[(indx + m2) >> 1].h);
                            float gvarv = epssq + gquinc[0] * Dgrb2[indx >> 1].v +
                                gquinc[1] *
                                    (Dgrb2[(indx - m1) >> 1].v + Dgrb2[(indx + p1) >> 1].v + Dgrb2[(indx - p1) >> 1].v +
                                     Dgrb2[(indx + m1) >> 1].v) +
                                gquinc[2] *
                                    (Dgrb2[(indx - v2) >> 1].v + Dgrb2[(indx - 2) >> 1].v + Dgrb2[(indx + 2) >> 1].v +
                                     Dgrb2[(indx + v2) >> 1].v) +
                                gquinc[3] *
                                    (Dgrb2[(indx - m2) >> 1].v + Dgrb2[(indx + p2) >> 1].v + Dgrb2[(indx - p2) >> 1].v +
                                     Dgrb2[(indx + m2) >> 1].v);

                            Dgrb[0][indx >> 1] = (hcd[indx] * gvarv + vcd[indx] * gvarh) / (gvarv + gvarh);
                            rgbgreen[indx] = cfa[indx] + Dgrb[0][indx >> 1];
                        }
                    }
                }
            }

            // ===== Diagonal gradients =====
            for (int rr = 6; rr < rr1 - 6; rr++) {
                if ((fc(rr, 2) & 1) == 0) {
                    for (int cc = 6, indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2) {
                        delp[indx >> 1] = fabsf(cfa[indx + p1] - cfa[indx - p1]);
                        delm[indx >> 1] = fabsf(cfa[indx + m1] - cfa[indx - m1]);
                        Dgrbsq1p[indx >> 1] = SQR(cfa[indx + 1] - cfa[indx + 1 - p1]) + SQR(cfa[indx + 1] - cfa[indx + 1 + p1]);
                        Dgrbsq1m[indx >> 1] = SQR(cfa[indx + 1] - cfa[indx + 1 - m1]) + SQR(cfa[indx + 1] - cfa[indx + 1 + m1]);
                    }
                } else {
                    for (int cc = 6, indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2) {
                        Dgrbsq1p[indx >> 1] = SQR(cfa[indx] - cfa[indx - p1]) + SQR(cfa[indx] - cfa[indx + p1]);
                        Dgrbsq1m[indx >> 1] = SQR(cfa[indx] - cfa[indx - m1]) + SQR(cfa[indx] - cfa[indx + m1]);
                        delp[indx >> 1] = fabsf(cfa[indx + 1 + p1] - cfa[indx + 1 - p1]);
                        delm[indx >> 1] = fabsf(cfa[indx + 1 + m1] - cfa[indx + 1 - m1]);
                    }
                }
            }

            // ===== Diagonal interpolation correction =====
            for (int rr = 8; rr < rr1 - 8; rr++) {
                for (int cc = 8 + (fc(rr, 2) & 1), indx = rr * ts + cc, indx1 = indx >> 1; cc < cc1 - 8; cc += 2, indx += 2, indx1++) {
                    float crse = 2.f * cfa[indx + m1] / (eps + cfa[indx] + cfa[indx + m2]);
                    float crnw = 2.f * cfa[indx - m1] / (eps + cfa[indx] + cfa[indx - m2]);
                    float crne = 2.f * cfa[indx + p1] / (eps + cfa[indx] + cfa[indx + p2]);
                    float crsw = 2.f * cfa[indx - p1] / (eps + cfa[indx] + cfa[indx - p2]);

                    float rbse = fabsf(1.f - crse) < arthresh ? cfa[indx] * crse : cfa[indx + m1] + 0.5f * (cfa[indx] - cfa[indx + m2]);
                    float rbnw = fabsf(1.f - crnw) < arthresh ? cfa[indx] * crnw : cfa[indx - m1] + 0.5f * (cfa[indx] - cfa[indx - m2]);
                    float rbne = fabsf(1.f - crne) < arthresh ? cfa[indx] * crne : cfa[indx + p1] + 0.5f * (cfa[indx] - cfa[indx + p2]);
                    float rbsw = fabsf(1.f - crsw) < arthresh ? cfa[indx] * crsw : cfa[indx - p1] + 0.5f * (cfa[indx] - cfa[indx - p2]);

                    float wtse = eps + delm[indx1] + delm[(indx + m1) >> 1] + delm[(indx + m2) >> 1];
                    float wtnw = eps + delm[indx1] + delm[(indx - m1) >> 1] + delm[(indx - m2) >> 1];
                    float wtne = eps + delp[indx1] + delp[(indx + p1) >> 1] + delp[(indx + p2) >> 1];
                    float wtsw = eps + delp[indx1] + delp[(indx - p1) >> 1] + delp[(indx - p2) >> 1];

                    rbm[indx1] = (wtse * rbnw + wtnw * rbse) / (wtse + wtnw);
                    rbp[indx1] = (wtne * rbsw + wtsw * rbne) / (wtne + wtsw);

                    float rbvarm = epssq +
                        gausseven[0] *
                            (Dgrbsq1m[(indx - v1) >> 1] + Dgrbsq1m[(indx - 1) >> 1] + Dgrbsq1m[(indx + 1) >> 1] + Dgrbsq1m[(indx + v1) >> 1]) +
                        gausseven[1] *
                            (Dgrbsq1m[(indx - v2 - 1) >> 1] + Dgrbsq1m[(indx - v2 + 1) >> 1] + Dgrbsq1m[(indx - 2 - v1) >> 1] +
                             Dgrbsq1m[(indx + 2 - v1) >> 1] + Dgrbsq1m[(indx - 2 + v1) >> 1] + Dgrbsq1m[(indx + 2 + v1) >> 1] +
                             Dgrbsq1m[(indx + v2 - 1) >> 1] + Dgrbsq1m[(indx + v2 + 1) >> 1]);

                    pmwt[indx1] = rbvarm /
                        ((epssq +
                          gausseven[0] *
                              (Dgrbsq1p[(indx - v1) >> 1] + Dgrbsq1p[(indx - 1) >> 1] + Dgrbsq1p[(indx + 1) >> 1] +
                               Dgrbsq1p[(indx + v1) >> 1]) +
                          gausseven[1] *
                              (Dgrbsq1p[(indx - v2 - 1) >> 1] + Dgrbsq1p[(indx - v2 + 1) >> 1] + Dgrbsq1p[(indx - 2 - v1) >> 1] +
                               Dgrbsq1p[(indx + 2 - v1) >> 1] + Dgrbsq1p[(indx - 2 + v1) >> 1] + Dgrbsq1p[(indx + 2 + v1) >> 1] +
                               Dgrbsq1p[(indx + v2 - 1) >> 1] + Dgrbsq1p[(indx + v2 + 1) >> 1])) +
                         rbvarm);

                    // Bound interpolation in regions of high saturation
                    if (rbp[indx1] < cfa[indx]) {
                        if (2.f * rbp[indx1] < cfa[indx]) {
                            rbp[indx1] = median3(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
                        } else {
                            float pwt = 2.f * (cfa[indx] - rbp[indx1]) / (eps + rbp[indx1] + cfa[indx]);
                            rbp[indx1] = pwt * rbp[indx1] + (1.f - pwt) * median3(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
                        }
                    }

                    if (rbm[indx1] < cfa[indx]) {
                        if (2.f * rbm[indx1] < cfa[indx]) {
                            rbm[indx1] = median3(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
                        } else {
                            float mwt = 2.f * (cfa[indx] - rbm[indx1]) / (eps + rbm[indx1] + cfa[indx]);
                            rbm[indx1] = mwt * rbm[indx1] + (1.f - mwt) * median3(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
                        }
                    }

                    if (rbp[indx1] > clipPt) {
                        rbp[indx1] = median3(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
                    }
                    if (rbm[indx1] > clipPt) {
                        rbm[indx1] = median3(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
                    }
                }
            }

            // ===== Combine plus/minus diagonal interpolations =====
            for (int rr = 10; rr < rr1 - 10; rr++) {
                for (int cc = 10 + (fc(rr, 2) & 1), indx = rr * ts + cc, indx1 = indx >> 1; cc < cc1 - 10; cc += 2, indx += 2, indx1++) {
                    float pmwtalt = 0.25f *
                        (pmwt[(indx - m1) >> 1] + pmwt[(indx + p1) >> 1] + pmwt[(indx - p1) >> 1] + pmwt[(indx + m1) >> 1]);

                    if (fabsf(0.5f - pmwt[indx1]) < fabsf(0.5f - pmwtalt)) {
                        pmwt[indx1] = pmwtalt;
                    }

                    rbint[indx1] = 0.5f * (cfa[indx] + rbm[indx1] * (1.f - pmwt[indx1]) + rbp[indx1] * pmwt[indx1]);
                }
            }

            // ===== Refine G using diagonal R+B =====
            for (int rr = 12; rr < rr1 - 12; rr++) {
                for (int cc = 12 + (fc(rr, 2) & 1), indx = rr * ts + cc, indx1 = indx >> 1; cc < cc1 - 12; cc += 2, indx += 2, indx1++) {
                    if (fabsf(0.5f - pmwt[indx >> 1]) < fabsf(0.5f - hvwt[indx >> 1])) {
                        continue;
                    }

                    float cru2 = cfa[indx - v1] * 2.0f / (eps + rbint[indx1] + rbint[indx1 - v1]);
                    float crd2 = cfa[indx + v1] * 2.0f / (eps + rbint[indx1] + rbint[indx1 + v1]);
                    float crl2 = cfa[indx - 1] * 2.0f / (eps + rbint[indx1] + rbint[indx1 - 1]);
                    float crr2 = cfa[indx + 1] * 2.0f / (eps + rbint[indx1] + rbint[indx1 + 1]);

                    float gu = fabsf(1.f - cru2) < arthresh ? rbint[indx1] * cru2 :
                                                              cfa[indx - v1] + 0.5f * (rbint[indx1] - rbint[indx1 - v1]);
                    float gd = fabsf(1.f - crd2) < arthresh ? rbint[indx1] * crd2 :
                                                              cfa[indx + v1] + 0.5f * (rbint[indx1] - rbint[indx1 + v1]);
                    float gl = fabsf(1.f - crl2) < arthresh ? rbint[indx1] * crl2 : cfa[indx - 1] + 0.5f * (rbint[indx1] - rbint[indx1 - 1]);
                    float gr = fabsf(1.f - crr2) < arthresh ? rbint[indx1] * crr2 : cfa[indx + 1] + 0.5f * (rbint[indx1] - rbint[indx1 + 1]);

                    float Gintv2 = (dirwts0[indx - v1] * gd + dirwts0[indx + v1] * gu) / (dirwts0[indx + v1] + dirwts0[indx - v1]);
                    float Ginth2 = (dirwts1[indx - 1] * gr + dirwts1[indx + 1] * gl) / (dirwts1[indx - 1] + dirwts1[indx + 1]);

                    if (Gintv2 < rbint[indx1]) {
                        if (2.f * Gintv2 < rbint[indx1]) {
                            Gintv2 = median3(Gintv2, cfa[indx - v1], cfa[indx + v1]);
                        } else {
                            float vwt2 = 2.0f * (rbint[indx1] - Gintv2) / (eps + Gintv2 + rbint[indx1]);
                            Gintv2 = vwt2 * Gintv2 + (1.f - vwt2) * median3(Gintv2, cfa[indx - v1], cfa[indx + v1]);
                        }
                    }

                    if (Ginth2 < rbint[indx1]) {
                        if (2.f * Ginth2 < rbint[indx1]) {
                            Ginth2 = median3(Ginth2, cfa[indx - 1], cfa[indx + 1]);
                        } else {
                            float hwt2 = 2.0f * (rbint[indx1] - Ginth2) / (eps + Ginth2 + rbint[indx1]);
                            Ginth2 = hwt2 * Ginth2 + (1.f - hwt2) * median3(Ginth2, cfa[indx - 1], cfa[indx + 1]);
                        }
                    }

                    if (Ginth2 > clipPt) {
                        Ginth2 = median3(Ginth2, cfa[indx - 1], cfa[indx + 1]);
                    }
                    if (Gintv2 > clipPt) {
                        Gintv2 = median3(Gintv2, cfa[indx - v1], cfa[indx + v1]);
                    }

                    rgbgreen[indx] = Ginth2 * (1.f - hvwt[indx1]) + Gintv2 * hvwt[indx1];
                    Dgrb[0][indx >> 1] = rgbgreen[indx] - cfa[indx];
                }
            }

            // ===== Fancy chrominance interpolation =====
            // Split G-B from G-R at B coset
            for (int rr = 13 - ey; rr < rr1 - 12; rr += 2) {
                for (int indx1 = (rr * ts + 13 - ex) >> 1; indx1 < (rr * ts + cc1 - 12) >> 1; indx1++) {
                    Dgrb[1][indx1] = Dgrb[0][indx1];
                    Dgrb[0][indx1] = 0;
                }
            }

            for (int rr = 14; rr < rr1 - 14; rr++) {
                for (int cc = 14 + (fc(rr, 2) & 1), indx = rr * ts + cc, c = 1 - fc(rr, cc) / 2; cc < cc1 - 14; cc += 2, indx += 2) {
                    float wtnw = 1.f /
                        (eps + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx + m1) >> 1]) +
                         fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx - m3) >> 1]) +
                         fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - m3) >> 1]));
                    float wtne = 1.f /
                        (eps + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx - p1) >> 1]) +
                         fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx + p3) >> 1]) +
                         fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + p3) >> 1]));
                    float wtsw = 1.f /
                        (eps + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + p1) >> 1]) +
                         fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + m3) >> 1]) +
                         fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx - p3) >> 1]));
                    float wtse = 1.f /
                        (eps + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - m1) >> 1]) +
                         fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - p3) >> 1]) +
                         fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx + m3) >> 1]));

                    Dgrb[c][indx >> 1] = (wtnw *
                                              (1.325f * Dgrb[c][(indx - m1) >> 1] - 0.175f * Dgrb[c][(indx - m3) >> 1] -
                                               0.075f * Dgrb[c][(indx - m1 - 2) >> 1] - 0.075f * Dgrb[c][(indx - m1 - v2) >> 1]) +
                                          wtne *
                                              (1.325f * Dgrb[c][(indx + p1) >> 1] - 0.175f * Dgrb[c][(indx + p3) >> 1] -
                                               0.075f * Dgrb[c][(indx + p1 + 2) >> 1] - 0.075f * Dgrb[c][(indx + p1 + v2) >> 1]) +
                                          wtsw *
                                              (1.325f * Dgrb[c][(indx - p1) >> 1] - 0.175f * Dgrb[c][(indx - p3) >> 1] -
                                               0.075f * Dgrb[c][(indx - p1 - 2) >> 1] - 0.075f * Dgrb[c][(indx - p1 - v2) >> 1]) +
                                          wtse *
                                              (1.325f * Dgrb[c][(indx + m1) >> 1] - 0.175f * Dgrb[c][(indx + m3) >> 1] -
                                               0.075f * Dgrb[c][(indx + m1 + 2) >> 1] - 0.075f * Dgrb[c][(indx + m1 + v2) >> 1])) /
                        (wtnw + wtne + wtsw + wtse);
                }
            }

            // ===== Output: write red, green, blue =====
            for (int rr = 16; rr < rr1 - 16; rr++) {
                int row = rr + top;
                if (row < 0 || row >= size.y()) {
                    continue;
                }

                int indx = rr * ts + 16;

                if ((fc(rr, 2) & 1) == 1) {
                    for (int col = left + 16; indx < rr * ts + cc1 - 16 - (cc1 & 1); indx++, col++) {
                        if (col < 0 || col >= size.x()) {
                            indx++;
                            col++;
                            continue;
                        }

                        float temp = 1.f /
                            (hvwt[(indx - v1) >> 1] + 2.f - hvwt[(indx + 1) >> 1] - hvwt[(indx - 1) >> 1] + hvwt[(indx + v1) >> 1]);
                        setRed(
                            row,
                            col,
                            rgbgreen[indx] -
                                ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1] +
                                 (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1] +
                                 (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1] +
                                 (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1]) *
                                    temp
                        );
                        setBlue(
                            row,
                            col,
                            rgbgreen[indx] -
                                ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1] +
                                 (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1] +
                                 (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1] +
                                 (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1]) *
                                    temp
                        );

                        indx++;
                        col++;
                        if (col < size.x()) {
                            setRed(row, col, rgbgreen[indx] - Dgrb[0][indx >> 1]);
                            setBlue(row, col, rgbgreen[indx] - Dgrb[1][indx >> 1]);
                        }
                    }

                    if (cc1 & 1) {
                        int col = left + cc1 - 16 - 1;
                        if (col >= 0 && col < size.x()) {
                            float temp = 1.f /
                                (hvwt[(indx - v1) >> 1] + 2.f - hvwt[(indx + 1) >> 1] - hvwt[(indx - 1) >> 1] + hvwt[(indx + v1) >> 1]);
                            setRed(
                                row,
                                col,
                                rgbgreen[indx] -
                                    ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1] +
                                     (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1] +
                                     (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1] +
                                     (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1]) *
                                        temp
                            );
                            setBlue(
                                row,
                                col,
                                rgbgreen[indx] -
                                    ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1] +
                                     (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1] +
                                     (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1] +
                                     (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1]) *
                                        temp
                            );
                        }
                    }
                } else {
                    for (int col = left + 16; indx < rr * ts + cc1 - 16 - (cc1 & 1); indx++, col++) {
                        if (col < 0 || col >= size.x()) {
                            indx++;
                            col++;
                            continue;
                        }

                        setRed(row, col, rgbgreen[indx] - Dgrb[0][indx >> 1]);
                        setBlue(row, col, rgbgreen[indx] - Dgrb[1][indx >> 1]);

                        indx++;
                        col++;
                        if (col < size.x()) {
                            float temp = 1.f /
                                (hvwt[(indx - v1) >> 1] + 2.f - hvwt[(indx + 1) >> 1] - hvwt[(indx - 1) >> 1] + hvwt[(indx + v1) >> 1]);
                            setRed(
                                row,
                                col,
                                rgbgreen[indx] -
                                    ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1] +
                                     (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1] +
                                     (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1] +
                                     (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1]) *
                                        temp
                            );
                            setBlue(
                                row,
                                col,
                                rgbgreen[indx] -
                                    ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1] +
                                     (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1] +
                                     (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1] +
                                     (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1]) *
                                        temp
                            );
                        }
                    }

                    if (cc1 & 1) {
                        int col = left + cc1 - 16 - 1;
                        if (col >= 0 && col < size.x()) {
                            setRed(row, col, rgbgreen[indx] - Dgrb[0][indx >> 1]);
                            setBlue(row, col, rgbgreen[indx] - Dgrb[1][indx >> 1]);
                        }
                    }
                }
            }

            // Copy green
            for (int rr = 16; rr < rr1 - 16; rr++) {
                int row = rr + top;
                if (row < 0 || row >= size.y()) {
                    continue;
                }

                for (int cc = 16; cc < cc1 - 16; cc++) {
                    int col = cc + left;
                    if (col >= 0 && col < size.x()) {
                        setGreen(row, col, rgbgreen[rr * ts + cc]);
                    }
                }
            }
        },
        priority
    );

    // ===== Border demosaicing (simple bilinear) =====
    if (border < 4) {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            (size_t)size.x() * size.y(),
            [&](int row) {
                for (int col = 0; col < size.x(); col++) {
                    if (row >= border && row < size.y() - border && col >= border && col < size.x() - border) {
                        continue;
                    }

                    int color = fc(row, col);
                    float sum[3] = {0.f, 0.f, 0.f};
                    int count[3] = {0, 0, 0};

                    for (int dr = -1; dr <= 1; dr++) {
                        for (int dc = -1; dc <= 1; dc++) {
                            int rr = std::clamp(row + dr, 0, size.y() - 1);
                            int cc = std::clamp(col + dc, 0, size.x() - 1);
                            int nc = fc(rr, cc);
                            sum[nc] += rawData(rr, cc);
                            count[nc]++;
                        }
                    }

                    float g = count[1] > 0 ? sum[1] / count[1] : 0.f;
                    float r = count[0] > 0 ? sum[0] / count[0] : 0.f;
                    float b = count[2] > 0 ? sum[2] / count[2] : 0.f;

                    float raw = rawData(row, col);
                    if (color == 0) {
                        r = raw;
                    } else if (color == 1) {
                        g = raw;
                    } else {
                        b = raw;
                    }

                    setRed(row, col, r);
                    setGreen(row, col, g);
                    setBlue(row, col, b);
                }
            },
            priority
        );
    }
}

} // namespace tev
