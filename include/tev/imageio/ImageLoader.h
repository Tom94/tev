/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <tev/Channel.h>
#include <tev/Colors.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/Simd.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/GainMap.h>

#include <nanogui/vector.h>

#include <xsimd/xsimd.hpp>

#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace tev {

template <bool SRGB_TO_LINEAR = false>
Task<void> yCbCrToRgb(
    MultiChannelView<float> data,
    int priority,
    const float offsets[2] = ituth273::DEFAULT_YCBCR_OFFSETS,
    const float coeffs[4] = ituth273::DEFAULT_YCBCR_COEFFS
) {
    if (data.nChannels() < 3) {
        tlog::warning("Cannot convert from YCbCr to RGB: not enough channels.");
        co_return;
    }

    const nanogui::Vector2i size = data.size();

    const auto numPixels = posProd(size);
    static constexpr auto TRANSFER = SRGB_TO_LINEAR ? ituth273::ETransfer::YCbCrSRGB : ituth273::ETransfer::YCbCrLinear;
    co_await simdParallelFor(
        ThreadPool::global(),
        0uz,
        numPixels,
        numPixels * 3 * ituth273::approxCost(TRANSFER),
        [&data, &offsets, &coeffs]<class B>(size_t i) {
            auto rgb = loadChannel<B, 3>(data, i);

            rgb = ituth273::yCbCrToRgb(rgb, offsets, coeffs);
            if constexpr (SRGB_TO_LINEAR) {
                rgb = ituth273::invTransferRgb<ituth273::ETransfer::SRGB>(rgb);
            }

            storeChannel<B>(rgb, data, i);
        },
        priority
    );
}

template <bool SRGB_TO_LINEAR = false> Task<void> yCbCrToRgbRct(MultiChannelView<float> data, int priority) {
    if (data.nChannels() < 3) {
        tlog::warning("Cannot convert from YCbCr to RGB: not enough channels.");
        co_return;
    }

    const nanogui::Vector2i size = data.size();

    const auto numPixels = posProd(size);
    static constexpr auto EFFECTIVE_TRANSFER = SRGB_TO_LINEAR ? ituth273::ETransfer::YCbCrSRGB : ituth273::ETransfer::YCbCrLinear;
    co_await simdParallelFor(
        ThreadPool::global(),
        0uz,
        numPixels,
        numPixels * 3 * ituth273::approxCost(EFFECTIVE_TRANSFER),
        [&data]<class B>(size_t i) {
            const auto y = loadChannel<B>(data, 0, i);
            const auto cb = loadChannel<B>(data, 1, i);
            const auto cr = loadChannel<B>(data, 2, i);

            auto g = y - ((cb + cr) / 4);
            nanogui::Array<B, 3> rgb = {
                cr + g,
                g,
                cb + g,
            };

            if constexpr (SRGB_TO_LINEAR) {
                rgb = ituth273::invTransferRgb<ituth273::ETransfer::SRGB>(rgb);
            }

            storeChannel<B>(rgb, data, i);
        },
        priority
    );
}

template <ituth273::ETransfer TRANSFER = ituth273::ETransfer::Linear, bool MULTIPLY_ALPHA = false, std::ranges::random_access_range T>
Task<void> toFloat32(
    T&& imageData,
    size_t numSamplesPerPixelIn,
    MultiChannelView<float> floatData,
    EAlphaKind alphaKind,
    int priority,
    // 0 defaults to 1/(2**bitsPerSample-1)
    float scale = 0.0f,
    float offset = 0.0f,
    // 0 defaults to numSamplesPerPixelIn * size.x()
    size_t numSamplesPerRowIn = 0
) {
    static constexpr size_t N = vf::size;

    using value_t = typename std::remove_cvref_t<T>::value_type;
    if constexpr (std::is_integral_v<value_t>) {
        if (scale == 0.0f) {
            scale = 1.0f / (float)std::numeric_limits<std::make_unsigned_t<value_t>>::max();
        }
    } else {
        if (scale == 0.0f) {
            scale = 1.0f;
        }
    }

    using namespace ituth273;

    const auto size = floatData.size();

    if (numSamplesPerRowIn == 0) {
        numSamplesPerRowIn = numSamplesPerPixelIn * size.x();
    }

    const size_t numSamplesPerPixel = std::min(numSamplesPerPixelIn, floatData.nChannels());
    const size_t numPixels = posProd(size);

    size_t expectedDataSize = numSamplesPerRowIn * size.y();
    if (alphaKind == EAlphaKind::None || !MULTIPLY_ALPHA) {
        // Don't require alpha channel (and channels before exceeding output channels) if not present or not multiplying, even if the input has it
        expectedDataSize = expectedDataSize - numSamplesPerPixelIn + numSamplesPerPixel;
    }

    if (imageData.size() < expectedDataSize) {
        throw std::runtime_error{
            fmt::format("Not enough image data provided: expected at least {} samples, got {}", expectedDataSize, imageData.size())
        };
    }

    const size_t numColorChannels = numSamplesPerPixel == 0 ? 0 : (numSamplesPerPixel - (alphaKind != EAlphaKind::None ? 1 : 0));
    const bool hasAlpha = alphaKind != EAlphaKind::None;

    static constexpr size_t DYNAMIC = 0;
    const auto numColorChannelsSpecialized = [&]<size_t N_COLOR_CHANNELS_STATIC>(size_t nColorChannelsDynamic) -> Task<void> {
        const size_t N_COLOR_CHANNELS = N_COLOR_CHANNELS_STATIC == DYNAMIC ? nColorChannelsDynamic : N_COLOR_CHANNELS_STATIC;
        co_await simdParallelFor(
            ThreadPool::global(),
            0uz,
            numPixels,
            numPixels * N * ituth273::approxCost(TRANSFER),
            [&]<class Scalar>(size_t idx) {
                const size_t y = (idx / (size_t)size.x());
                const size_t x = (idx - y * (size_t)size.x());

                const size_t rowIdxIn = y * numSamplesPerRowIn;
                const size_t alphaOff = numSamplesPerPixelIn - 1;

                // A tiny set of if-constexpr branches handle the load/store/select
                // differences; the arithmetic is written once.
                const auto loadChannel = [&](size_t c) -> Scalar {
                    if constexpr (std::is_same_v<Scalar, float>) {
                        const size_t base = rowIdxIn + (size_t)x * numSamplesPerPixelIn;
                        return (float)imageData[base + c] * scale + offset;
                    } else {
                        alignas(vf::arch_type::alignment()) float in[N];
                        for (size_t i = 0; i < N; ++i) {
                            const size_t base = rowIdxIn + (x + i) * numSamplesPerPixelIn;
                            in[i] = (float)imageData[base + c];
                        }

                        return xsimd::fma(vf::load_aligned(in), vf(scale), vf(offset));
                    }
                };

                // Alpha (needed before the color loop for premultiply handling).
                Scalar a = hasAlpha ? loadChannel(alphaOff) : Scalar(1.0f);

                Scalar factor = Scalar(1.0f);
                Scalar invFactor = Scalar(1.0f);
                if constexpr (MULTIPLY_ALPHA) {
                    if (alphaKind == EAlphaKind::PremultipliedNonlinear) {
                        factor = xsimd::select(a > Scalar(0.0001f), Scalar(1.0f / a), Scalar(1.0f));
                        invFactor = a;
                    } else if (alphaKind == EAlphaKind::Straight) {
                        invFactor = a;
                    }
                }

                if (N_COLOR_CHANNELS >= 3) {
                    nanogui::Array<Scalar, 3> rgb = {
                        loadChannel(0) * factor,
                        loadChannel(1) * factor,
                        loadChannel(2) * factor,
                    };

                    rgb = ituth273::invTransferRgb<TRANSFER>(rgb);
                    for (size_t c = 0; c < 3; ++c) {
                        storeChannel(floatData, c, x, y, rgb[c] * invFactor);
                    }

                    for (size_t c = 3; c < N_COLOR_CHANNELS; ++c) {
                        Scalar v = loadChannel(c) * factor;
                        v = ituth273::invTransferComponent<TRANSFER>(v);
                        storeChannel(floatData, c, x, y, v * invFactor);
                    }
                } else {
                    for (size_t c = 0; c < N_COLOR_CHANNELS; ++c) {
                        Scalar v = loadChannel(c) * factor;
                        v = ituth273::invTransferComponent<TRANSFER>(v);
                        storeChannel(floatData, c, x, y, v * invFactor);
                    }
                }

                if (hasAlpha) {
                    storeChannel(floatData, -1, x, y, a);
                }
            },
            priority
        );
    };

    // Specialize for 3 channels (RGB) to get extra instruction-level parallelism (unrolled loop over channels). 1 channel wouldn't benefit
    // (nothing to unroll), 2 (e.g. uv) and 4 (e.g. CMYK) color channels are rare enough that it's not worth the binary size increase.
    if (numColorChannels == 3) {
        co_await numColorChannelsSpecialized.template operator()<3>(3);
    } else {
        co_await numColorChannelsSpecialized.template operator()<DYNAMIC>(numColorChannels);
    }
}

template <bool MULTIPLY_ALPHA = false, std::ranges::random_access_range T>
Task<void> toFloat32(
    ituth273::ETransfer transfer,
    T&& imageData,
    size_t numSamplesPerPixelIn,
    MultiChannelView<float> floatData,
    EAlphaKind alphaKind,
    int priority,
    // 0 defaults to 1/(2**bitsPerSample-1)
    float scale = 0.0f,
    float offset = 0.0f,
    // 0 defaults to numSamplesPerPixelIn * size.x()
    size_t numSamplesPerRowIn = 0
) {
    using namespace ituth273;
    switch (transfer) {
        case ETransfer::BT709:
            co_return co_await toFloat32<ETransfer::BT709, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::BT601:
            co_return co_await toFloat32<ETransfer::BT601, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::BT202010bit:
            co_return co_await toFloat32<ETransfer::BT202010bit, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::BT202012bit:
            co_return co_await toFloat32<ETransfer::BT202012bit, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::IEC61966_2_4:
            co_return co_await toFloat32<ETransfer::IEC61966_2_4, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::BT1361Extended:
            co_return co_await toFloat32<ETransfer::BT1361Extended, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::Gamma22:
            co_return co_await toFloat32<ETransfer::Gamma22, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::Gamma28:
            co_return co_await toFloat32<ETransfer::Gamma28, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::SMPTE240:
            co_return co_await toFloat32<ETransfer::SMPTE240, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::Linear:
            co_return co_await toFloat32<ETransfer::Linear, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::Log100:
            co_return co_await toFloat32<ETransfer::Log100, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::Log100Sqrt10:
            co_return co_await toFloat32<ETransfer::Log100Sqrt10, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::SRGB:
            co_return co_await toFloat32<ETransfer::SRGB, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::PQ:
            co_return co_await toFloat32<ETransfer::PQ, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::SMPTE428:
            co_return co_await toFloat32<ETransfer::SMPTE428, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::HLG:
            co_return co_await toFloat32<ETransfer::HLG, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::Unspecified:
            co_return co_await toFloat32<ETransfer::Unspecified, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::LUT:
            co_return co_await toFloat32<ETransfer::LUT, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        case ETransfer::GenericGamma:
            co_return co_await toFloat32<ETransfer::GenericGamma, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
        // Fall back to linear otherwise
        default:
            co_return co_await toFloat32<ETransfer::Linear, MULTIPLY_ALPHA>(
                imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
            );
    }
}

template <std::ranges::random_access_range T>
Task<void> toFloat32(
    ituth273::ETransfer transfer,
    bool multiplyAlpha,
    T&& imageData,
    size_t numSamplesPerPixelIn,
    MultiChannelView<float> floatData,
    EAlphaKind alphaKind,
    int priority,
    // 0 defaults to 1/(2**bitsPerSample-1)
    float scale = 0.0f,
    float offset = 0.0f,
    // 0 defaults to numSamplesPerPixelIn * size.x()
    size_t numSamplesPerRowIn = 0
) {
    using namespace ituth273;
    if (multiplyAlpha) {
        co_return co_await toFloat32<true>(
            transfer, imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
        );
    } else {
        co_return co_await toFloat32<false>(
            transfer, imageData, numSamplesPerPixelIn, floatData, alphaKind, priority, scale, offset, numSamplesPerRowIn
        );
    }
}

struct ImageLoaderSettings {
    GainmapHeadroom gainmapHeadroom = {};
    bool dngApplyCameraProfile = false;
};

class ImageLoader {
public:
    class FormatNotSupported final : public std::runtime_error {
    public:
        FormatNotSupported(const std::string& message) : std::runtime_error{message} {}
    };

    virtual ~ImageLoader() {}

    virtual Task<std::vector<ImageData>> load(
        std::istringstream& iStream, const fs::path& path, std::string_view channelSelector, const ImageLoaderSettings& settings, int priority
    ) const = 0;

    virtual std::string name() const = 0;

    static const std::vector<std::unique_ptr<ImageLoader>>& getLoaders();

    // Returns a list of all supported mime types, sorted by decoding preference.
    static const std::vector<std::string_view>& supportedMimeTypes();

    static Task<std::vector<Channel>> makeInterleavedChannels(
        size_t numChannels,
        size_t numInterleavedDims,
        bool hasAlpha,
        nanogui::Vector2i size,
        EPixelFormat pixelFormat,
        EPixelFormat desiredFormat,
        std::string_view layer,
        int priority
    );

    static std::vector<Channel> makeNChannels(
        size_t numChannels, nanogui::Vector2i size, EPixelFormat pixelFormat, EPixelFormat desiredFormat, std::string_view layer
    );

    static Task<void> resizeChannelsAsync(
        std::span<const Channel> srcChannels, std::span<Channel> dstChannels, const std::optional<Box2i>& dstBox, int priority
    );
    static Task<void> resizeImageData(ImageData& resultData, nanogui::Vector2i targetSize, const std::optional<Box2i>& targetBox, int priority);
};

} // namespace tev
