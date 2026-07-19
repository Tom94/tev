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
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/GainMap.h>

#include <nanogui/vector.h>

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
    nanogui::Vector2f offsets = {0.5f, 0.5f},
    nanogui::Vector4f coeffs = {1.402f, -0.344136f, -0.714136f, 1.772f}
) {
    if (data.nChannels() < 3) {
        tlog::warning("Cannot convert from YCbCr to RGB: not enough channels.");
        co_return;
    }

    const nanogui::Vector2i size = data.size();

    const auto numPixels = posProd(size);
    co_await ThreadPool::global().parallelFor(
        0uz,
        numPixels,
        numPixels * 3,
        [offsets, &coeffs, &data](size_t i) {
            const float Y = data[0, i];
            const float Cb = data[1, i] - offsets[0];
            const float Cr = data[2, i] - offsets[1];

            // BT.601 conversion
            float r = Y + coeffs[0] * Cr;
            float g = Y + coeffs[1] * Cb + coeffs[2] * Cr;
            float b = Y + coeffs[3] * Cb;

            if constexpr (SRGB_TO_LINEAR) {
                r = toLinear(r);
                g = toLinear(g);
                b = toLinear(b);
            }

            data[0, i] = r;
            data[1, i] = g;
            data[2, i] = b;
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
    co_await ThreadPool::global().parallelFor(
        0uz,
        numPixels,
        numPixels * 3,
        [&data](size_t i) {
            const float Y = data[0, i];
            const float Cb = data[1, i];
            const float Cr = data[2, i];

            float g = Y - ((Cb + Cr) / 4);
            float r = Cr + g;
            float b = Cb + g;

            if constexpr (SRGB_TO_LINEAR) {
                r = toLinear(r);
                g = toLinear(g);
                b = toLinear(b);
            }

            data[0, i] = r;
            data[1, i] = g;
            data[2, i] = b;
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

    // N == 0 means "use runtime count"
    constexpr size_t DYNAMIC = 0;
    const auto channelCountSpecialized = [&]<size_t N_STATIC>(const size_t nDynamic) -> Task<void> {
        const size_t N = N_STATIC == DYNAMIC ? nDynamic : N_STATIC;
        const size_t N3 = std::min(N, 3uz);
        co_await ThreadPool::global().parallelFor(
            0,
            size.y(),
            numPixels * numSamplesPerPixel,
            [&](int y) {
                const size_t rowIdxIn = y * numSamplesPerRowIn;

                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdxIn = rowIdxIn + x * numSamplesPerPixelIn;

                    const float alpha = alphaKind != EAlphaKind::None ?
                        (float)imageData[baseIdxIn + numSamplesPerPixelIn - 1] * scale + offset :
                        1.0f;
                    float factor = 1.0f, invFactor = 1.0f;
                    if constexpr (MULTIPLY_ALPHA) {
                        factor = alphaKind == EAlphaKind::PremultipliedNonlinear && alpha > 0.0001f ? 1.0f / alpha : 1.0f;
                        invFactor = alphaKind == EAlphaKind::PremultipliedNonlinear || alphaKind == EAlphaKind::Straight ? alpha : 1.0f;
                    }

                    if constexpr (TRANSFER == ETransfer::HLG) {
                        nanogui::Vector3f result;
                        for (size_t c = 0; c < N3; ++c) {
                            result[c] = imageData[baseIdxIn + c] * scale + offset;
                        }

                        result = ituth273::invTransfer<TRANSFER>(result * factor) * invFactor;

                        for (size_t c = 0; c < N3; ++c) {
                            floatData[c, x, y] = result[c];
                        }
                    } else {
                        for (size_t c = 0; c < N; ++c) {
                            float result = imageData[baseIdxIn + c] * scale + offset;
                            result = ituth273::invTransferComponent<TRANSFER>(result * factor) * invFactor;
                            floatData[c, x, y] = result;
                        }
                    }

                    if (alphaKind != EAlphaKind::None) {
                        floatData[-1, x, y] = alpha;
                    }
                }
            },
            priority
        );
    };

    const size_t numColorChannels = numSamplesPerPixel == 0 ? 0 : (numSamplesPerPixel - (alphaKind != EAlphaKind::None ? 1 : 0));

    // Specialize for the common cases of 1 and 3 color channels (around 10% faster at the cost of binary size). 2 channel and 4 channel
    // (CMYK) images are rare enough that we don't care enough about the 10% speed-up to justify the binary size increase.
    if (numColorChannels == 1) {
        co_await channelCountSpecialized.template operator()<1>(1);
    } else if (numColorChannels == 3) {
        co_await channelCountSpecialized.template operator()<3>(3);
    } else {
        co_await channelCountSpecialized.template operator()<DYNAMIC>(numColorChannels);
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
