/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
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

#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Demosaic.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/Jpeg2000ImageLoader.h>
#include <tev/imageio/JxlImageLoader.h>
#include <tev/imageio/TiffImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <half.h>
#include <jpeglib.h>
#include <tiff.h>
#include <tiffio.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

enum class ETiffKind {
    U32 = 0,
    I32,
    F16,
    F24,
    F32,
    F64,
    Palette,
};

string toString(ETiffKind kind) {
    switch (kind) {
        case ETiffKind::U32: return "U32";
        case ETiffKind::I32: return "I32";
        case ETiffKind::F16: return "F16";
        case ETiffKind::F24: return "F24";
        case ETiffKind::F32: return "F32";
        case ETiffKind::F64: return "F64";
        case ETiffKind::Palette: return "Palette";
        default: throw runtime_error{"Unknown TIFF kind."};
    }
}

template <typename T> constexpr TIFFDataType tiffDataType() {
    if constexpr (std::is_same_v<T, float>) {
        return TIFF_FLOAT;
    } else if constexpr (std::is_same_v<T, double>) {
        return TIFF_DOUBLE;
    } else if constexpr (std::is_same_v<T, uint8_t>) {
        return TIFF_BYTE;
    } else if constexpr (std::is_same_v<T, uint16_t>) {
        return TIFF_SHORT;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return TIFF_LONG;
    } else if constexpr (std::is_same_v<T, uint64_t>) {
        return TIFF_LONG8;
    } else if constexpr (std::is_same_v<T, int8_t>) {
        return TIFF_SBYTE;
    } else if constexpr (std::is_same_v<T, int16_t>) {
        return TIFF_SSHORT;
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return TIFF_SLONG;
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return TIFF_SLONG8;
    } else if constexpr (std::is_same_v<T, char>) {
        return TIFF_ASCII;
    } else {
        static_assert(!sizeof(T), "unsupported TIFF type");
    }
}

template <typename T> span<const T> tiffGetSpan(TIFF* tif, ttag_t tag) {
    const TIFFField* field = TIFFFindField(tif, tag, TIFF_ANY);
    if (!field) {
        return {};
    }

    if (const int size = TIFFFieldSetGetSize(field); size != sizeof(T)) {
        tlog::warning() << fmt::format("TIFF tag {} has unexpected size (expected {}, got {})", tag, sizeof(tiffDataType<T>()), size);
        return {};
    }

    const int countSize = TIFFFieldSetGetCountSize(field);
    if (countSize == 0) {
        // Fixed-count tag. ReadCount gives the number of elements.
        const int n = TIFFFieldReadCount(field);
        if (n <= 0) {
            return {};
        }

        const T* data = nullptr;
        if (n == 1) {
            tlog::warning() << fmt::format("TIFF tag {} is a single value, but expected an array. Skipping.", tag);
            return {};
        }

        if (TIFFGetField(tif, tag, &data) && data) {
            return {data, static_cast<size_t>(n)};
        }
    } else if (countSize == 2) {
        uint16_t count;
        const T* data;
        if (TIFFGetField(tif, tag, &count, &data) && data) {
            return {data, count};
        }
    } else if (countSize == 4) {
        uint32_t count;
        const T* data;
        if (TIFFGetField(tif, tag, &count, &data) && data) {
            return {data, count};
        }
    } else if (countSize == 8) {
        uint64_t count;
        const T* data;
        if (TIFFGetField(tif, tag, &count, &data) && data) {
            return {data, count};
        }
    }

    return {};
}

template <typename T> optional<T> tiffGetValue(TIFF* tif, ttag_t tag) {
    const TIFFField* field = TIFFFindField(tif, tag, TIFF_ANY);
    if (!field) {
        return nullopt;
    }

    if (const int size = TIFFFieldSetGetSize(field); size != sizeof(T)) {
        tlog::warning() << fmt::format("TIFF tag {} has unexpected size (expected {}, got {})", tag, sizeof(tiffDataType<T>()), size);
        return nullopt;
    }

    const int countSize = TIFFFieldSetGetCountSize(field);
    if (countSize > 0) {
        const auto asSpan = tiffGetSpan<T>(tif, tag);
        if (asSpan.size() == 1) {
            return asSpan[0];
        } else {
            tlog::warning()
                << fmt::format("TIFF tag {} is an array of {} elements, but expected a single value. Skipping.", tag, asSpan.size());
            return nullopt;
        }
    } else if (countSize == 0) {
        T value;
        if (TIFFGetField(tif, tag, &value)) {
            return value;
        }
    } else {
        tlog::warning() << fmt::format("TIFF tag {} has unsupported count size {}. Skipping.", tag, countSize);
        return nullopt;
    }

    return nullopt;
}

template <typename T> array<span<const T>, 3> tiffGetRgbSpans(TIFF* tif, ttag_t tag, size_t elementsPerArray) {
    array<span<const T>, 3> result = {};

    const TIFFField* field = TIFFFindField(tif, tag, TIFF_ANY);
    if (!field) {
        return {};
    }

    // TIFFGetField expects one pointer arg per array.
    // These tags only ever return 1 or 3 arrays in practice.
    const T* ptrs[3] = {};
    if (!TIFFGetField(tif, tag, &ptrs[0], &ptrs[1], &ptrs[2])) {
        return {};
    }

    for (size_t i = 0; i < 3; i++) {
        if (ptrs[i]) {
            result[i] = {ptrs[i], elementsPerArray};
        }
    }

    return result;
}

array<span<const uint16_t>, 3> tiffGetTransferFunction(TIFF* tif) {
    uint16_t bitsPerSample = 8;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
    size_t n = 1u << bitsPerSample;
    return tiffGetRgbSpans<uint16_t>(tif, TIFFTAG_TRANSFERFUNCTION, n);
}

array<span<const uint16_t>, 3> tiffGetColorMap(TIFF* tif) {
    uint16_t bitsPerSample = 8;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
    const size_t n = 1u << bitsPerSample;
    return tiffGetRgbSpans<uint16_t>(tif, TIFFTAG_COLORMAP, n);
}

Task<void> convertF16AndF24ToF32(ETiffKind kind, uint32_t* __restrict imageData, size_t numSppIn, const nanogui::Vector2i& size, int priority) {
    if (kind == ETiffKind::F16) {
        size_t numSamples = (size_t)size.x() * size.y() * numSppIn;
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0, numSamples, numSamples, [&](size_t i) { *(float*)&imageData[i] = *(half*)&imageData[i]; }, priority
        );

        kind = ETiffKind::F32;
    } else if (kind == ETiffKind::F24) {
        size_t numSamples = (size_t)size.x() * size.y() * numSppIn;
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numSamples,
            numSamples,
            [&](size_t i) {
                uint32_t packed = imageData[i];
                // 1-7-16 layout:
                uint32_t sign = (packed >> 23) & 0x1;      // 1-bit sign
                uint32_t exponent = (packed >> 16) & 0x7F; // 7-bit exponent
                uint16_t mantissa = packed & 0xFFFF;       // 16-bit mantissa
                // Convert to ieee (1-8-23 layout):
                uint32_t ieee_exponent = exponent == 0 ? 0 : (exponent - 63 + 127); // Twice the bias range
                uint32_t ieee_mantissa = mantissa << 7;                             // Pad with 7 zeros
                imageData[i] = (sign << 31) | (ieee_exponent << 23) | ieee_mantissa;
            },
            priority
        );

        kind = ETiffKind::F32;
    } else {
        throw ImageLoadError{fmt::format("Unsupported TIFF kind for F16/F24 conversion: {}", toString(kind))};
    }
}

template <bool SRGB_TO_LINEAR = false>
Task<void> tiffDataToFloat32(
    ETiffKind kind,
    const Vector2i& interleave,
    const array<span<const uint16_t>, 3>& palette,
    uint32_t* __restrict imageData,
    size_t numSppIn,
    const MultiChannelView<float>& rgbaView,
    bool hasAlpha,
    int priority,
    float scale,
    bool flipWhiteAndBlack
) {
    const auto size = rgbaView.size();
    const auto numPixels = (size_t)size.x() * size.y();

    if (kind == ETiffKind::F64) {
        co_await toFloat32<double, SRGB_TO_LINEAR>((const double*)imageData, numSppIn, rgbaView, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::F32) {
        co_await toFloat32<float, SRGB_TO_LINEAR>((const float*)imageData, numSppIn, rgbaView, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::I32) {
        co_await toFloat32<int32_t, SRGB_TO_LINEAR>((const int32_t*)imageData, numSppIn, rgbaView, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::U32) {
        co_await toFloat32<uint32_t, SRGB_TO_LINEAR>((const uint32_t*)imageData, numSppIn, rgbaView, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::Palette) {
        if (any_of(palette.begin(), palette.end(), [](const auto& c) { return c.empty(); })) {
            throw runtime_error{"Palette data is empty."};
        }

        if (rgbaView.nChannels() < 3) {
            throw runtime_error{"Number of output samples per pixel must be at least 3 for palette data."};
        }

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * rgbaView.nChannels(),
            [&](size_t i) {
                const size_t index = imageData[i * numSppIn];
                const float paletteScale = 1.0f / 65535.0f;
                for (size_t c = 0; c < 3; ++c) {
                    const size_t localIdx = std::clamp(index, (size_t)0, palette[c].size() - 1);
                    rgbaView[c, i] = palette[c][localIdx] * paletteScale;
                }

                for (size_t c = 3, count = std::min(rgbaView.nChannels(), numSppIn + 2); c < count; ++c) {
                    rgbaView[c, i] = imageData[i * numSppIn + c - 2] * scale;
                }
            },
            priority
        );
    } else {
        throw ImageLoadError{fmt::format("Unsupported sample format: {}", toString(kind))};
    }

    if (flipWhiteAndBlack) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * rgbaView.nChannels(),
            [&](size_t i) {
                for (size_t c = 0, count = rgbaView.nChannels(); c < count; ++c) {
                    rgbaView[c, i] = 1.0f - rgbaView[c, i];
                }
            },
            priority
        );
    }
}

// Custom TIFF error and warning handlers to avoid console output
static void tiffErrorHandler(const char* module, const char* fmt, va_list args) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    tlog::warning() << fmt::format("TIFF error ({}): {}", module ? module : "unknown", buffer);
}

static void tiffWarningHandler(const char* module, const char* fmt, va_list args) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    tlog::warning() << fmt::format("TIFF warning ({}): {}", module ? module : "unknown", buffer);
}

// Custom TIFF I/O functions for reading from an istream
struct TiffData {
    TiffData(const uint8_t* data, size_t size) : data(data), offset(0), size(size) {}

    const uint8_t* data;
    toff_t offset;
    tsize_t size;
};

static tsize_t tiffReadProc(thandle_t handle, tdata_t data, tsize_t size) {
    auto tiffData = static_cast<TiffData*>(handle);
    size = std::min(size, tiffData->size - (tsize_t)tiffData->offset);
    memcpy(data, tiffData->data + tiffData->offset, size);
    tiffData->offset += size;
    return size;
}

static tsize_t tiffWriteProc(thandle_t, tdata_t, tsize_t) {
    return 0; // We don't need to write
}

static toff_t tiffSeekProc(thandle_t handle, toff_t offset, int whence) {
    auto tiffData = static_cast<TiffData*>(handle);

    switch (whence) {
        case SEEK_SET: tiffData->offset = offset; break;
        case SEEK_CUR: tiffData->offset += offset; break;
        case SEEK_END: tiffData->offset = tiffData->size - offset; break;
    }

    return tiffData->offset;
}

static int tiffCloseProc(thandle_t) {
    return 0; // We don't need to close the stream
}

static toff_t tiffSizeProc(thandle_t handle) {
    auto tiffData = static_cast<TiffData*>(handle);
    return tiffData->size;
}

static int tiffMapProc(thandle_t handle, tdata_t* pdata, toff_t* psize) {
    // We're not actually using memory mapping -- merely passing a pointer to the in-memory file data.
    auto tiffData = static_cast<TiffData*>(handle);
    *pdata = (tdata_t)tiffData->data;
    *psize = tiffData->size;
    return 1;
}

static void tiffUnmapProc(thandle_t, tdata_t, toff_t) {
    // No need to unmap, as we are not actually using memory mapping.
}

// https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_0_0.pdf page 94
float dngHdrEncodingFunction(const float x) { return x * (256.0f + x) / (256.0f * (1 + x)); }
float dngHdrDecodingFunction(const float x) { return 16.0f * (8.0f * x - 8.0f + sqrt(64.0f * x * x - 127.0f * x + 64.0f)); }

template <typename T>
void unpackBits(
    const uint8_t* const __restrict input,
    const size_t inputSize,
    const int bitwidth,
    T* const __restrict output,
    const size_t outputSize,
    const bool handleSign
) {
    // If the bitwidth is byte aligned (multiple of 8), libtiff already arranged the data in our machine's endianness
    if (bitwidth % 8 == 0) {
        const uint32_t bytesPerSample = bitwidth / 8;

        for (size_t i = 0; i < outputSize; ++i) {
            output[i] = 0;
            for (uint32_t j = 0; j < bytesPerSample; ++j) {
                if constexpr (endian::native == endian::little) {
                    output[i] |= (T)input[i * bytesPerSample + j] << (8 * j);
                } else {
                    output[i] |= (T)input[i * bytesPerSample + j] << ((sizeof(T) * 8 - 8) - 8 * j);
                }
            }

            // If signbit is set, set all bits to the left to 1
            if (handleSign && (output[i] & (1ull << (bitwidth - 1)))) {
                output[i] |= ~(T)((1ull << bitwidth) - 1);
            }
        }

        return;
    }

    // Otherwise, the data is packed in a bitwise, MSB first / big endian manner
    uint64_t currentBits = 0;
    int bitsAvailable = 0;
    size_t i = 0;

    for (size_t j = 0; j < inputSize; ++j) {
        currentBits = (currentBits << 8) | input[j];
        bitsAvailable += 8;

        while (bitsAvailable >= bitwidth && i < outputSize) {
            bitsAvailable -= bitwidth;
            output[i] = (currentBits >> bitsAvailable) & ((1ull << bitwidth) - 1);

            // If signbit is set, set all bits to the left to 1
            if (handleSign && (output[i] & (1ull << (bitwidth - 1)))) {
                output[i] |= ~(T)((1ull << bitwidth) - 1);
            }

            ++i;
        }
    }
}

Box2i getActiveArea(TIFF* tif, const Vector2i& size) {
    Box2i activeArea{Vector2i(0, 0), size};

    if (const auto aa = tiffGetSpan<uint32_t>(tif, TIFFTAG_ACTIVEAREA); aa.size() >= 4) {
        activeArea.min.y() = aa[0];
        activeArea.min.x() = aa[1];
        activeArea.max.y() = aa[2];
        activeArea.max.x() = aa[3];
    }

    if (!activeArea.isValid() || !Box2i(Vector2i{0, 0}, size).contains(activeArea)) {
        tlog::warning() << fmt::format("Invalid active area: min={} max={}; using full image area instead.", activeArea.min, activeArea.max);
        return Box2i{
            Vector2i{0, 0},
            size
        };
    }

    return activeArea;
}

// Per DNG spec: relative to top-left corner of active area!
Box2i getDefaultCrop(TIFF* tif, const Vector2i& size) {
    Box2i cropBox{Vector2i(0, 0), size};

    if (const auto origin = tiffGetSpan<float>(tif, TIFFTAG_DEFAULTCROPORIGIN); origin.size() >= 2) {
        cropBox.min.x() = (int)origin[0];
        cropBox.min.y() = (int)origin[1];
    }

    if (const auto size = tiffGetSpan<float>(tif, TIFFTAG_DEFAULTCROPSIZE); size.size() >= 2) {
        cropBox.max.x() = cropBox.min.x() + (int)size[0];
        cropBox.max.y() = cropBox.min.y() + (int)size[1];
    }

    if (!cropBox.isValid() || !Box2i(Vector2i{0, 0}, size).contains(cropBox)) {
        tlog::warning() << fmt::format("Invalid crop area: min={} max={}; using full area instead.", cropBox.min, cropBox.max);
        return Box2i{Vector2i(0, 0), size};
    }

    return cropBox;
}

Task<void> demosaicCfa(TIFF* tif, ChannelView<float> cfaData, const MultiChannelView<float>& rgbData, int priority) {
    if (rgbData.nChannels() < 3) {
        throw ImageLoadError{fmt::format("RGB output must have at least 3 channels, got {}", rgbData.nChannels())};
    }

    if (cfaData.size() != rgbData.size()) {
        throw ImageLoadError{
            fmt::format("CFA and RGB data must have the same size. Got CFA size {} and RGB size {}.", cfaData.size(), rgbData.size())
        };
    }

    // With CFA sensors, it's often the case that differently colored pixels have different sensitivities (captured by white balance), and,
    // as such, RGB==1 doesn't actually correspond to white after conversion to a display color space. To avoid this, we perform demosaicing
    // in a sort of white-divided space with values clipped to [0,1], which has the effect of clipping highlights to display-white as well
    // as preventing colored haloes due to a mitmatch with demosaicing heuristics. See also
    // https://github.com/CarVac/librtprocess/blob/master/src/demosaic/amaze.cc#L225
    Vector3f wbScale = {1.0f};

    {
        const uint64_t prevOffset = TIFFCurrentDirOffset(tif);
        TIFFSetDirectory(tif, 0);
        const ScopeGuard guard{[&]() { TIFFSetSubDirectory(tif, prevOffset); }};

        if (const auto asn = tiffGetSpan<float>(tif, TIFFTAG_ASSHOTNEUTRAL); asn.size() >= 3) {
            const float maxVal = std::max({asn[0], asn[1], asn[2]});
            wbScale = Vector3f{asn[0], asn[1], asn[2]} / maxVal;
            tlog::debug() << fmt::format("Clipping integer CFA to neutral white {}", wbScale);
        }
    }

    const Vector3f invWbScale = Vector3f{1.0f} / wbScale;

    const auto dim = tiffGetSpan<uint16_t>(tif, TIFFTAG_EP_CFAREPEATPATTERNDIM);
    if (dim.size() != 2 || dim[0] == 0 || dim[1] == 0) {
        throw ImageLoadError{fmt::format(
            "Invalid CFA pattern dimensions: expected 2 positive values, got {}",
            dim.size() == 2 ? fmt::format("{}, {}", dim[0], dim[1]) : fmt::format("{}", dim.size())
        )};
    }

    const auto cfaSize = Vector2i{dim[1], dim[0]};
    const size_t patternSize = (size_t)cfaSize.x() * cfaSize.y();

    const auto pat = tiffGetSpan<uint8_t>(tif, TIFFTAG_EP_CFAPATTERN);
    if (pat.size() < patternSize) {
        throw ImageLoadError{fmt::format("CFA pattern size is smaller than expected: expected at least {}, got {}", patternSize, pat.size())};
    }

    enum class ELayout : uint16_t { Rect = 1, A, B, C, D, E, F, G, H, I };
    const ELayout layout = tiffGetValue<ELayout>(tif, TIFFTAG_CFALAYOUT).value_or(ELayout::Rect);

    if (layout != ELayout::Rect) {
        tlog::warning() << fmt::format("Found CFALayout tag with non-rectangular value {}; not supported yet", (uint16_t)layout);
    }

    tlog::debug() << fmt::format("Found CFA pattern of size {}; applying...", cfaSize);

    const auto size = cfaData.size();
    const auto numPixels = (size_t)size.x() * size.y();

    if (wbScale != Vector3f{1.0f}) {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels,
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    cfaData[x, y] =
                        std::clamp(cfaData[x, y] * invWbScale[pat[(y % cfaSize.y()) * cfaSize.x() + (x % cfaSize.x())]], 0.0f, 1.0f);
                }
            },
            priority
        );
    }

    co_await demosaic(cfaData, rgbData, pat, cfaSize, priority);

    if (wbScale != Vector3f{1.0f}) {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels,
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    for (int c = 0; c < 3; ++c) {
                        rgbData[c, x, y] *= wbScale[c];
                    }
                }
            },
            priority
        );
    }
}

Task<void> linearizeAndNormalizeRawDng(
    TIFF* tif, const uint16_t dataSampleFormat, const uint16_t dataBitsPerSample, const MultiChannelView<float>& rgbaView, const int priority
) {
    const auto numChannels = rgbaView.nChannels();

    const auto size = rgbaView.size();
    const auto numPixels = (size_t)size.x() * size.y();

    // Utility var that we'll reuse whenever reading a variable TIFF array
    const size_t maxVal = dataSampleFormat != SAMPLEFORMAT_IEEEFP ? (1ull << dataBitsPerSample) - 1 : 1.0f;
    const float scale = 1.0f / maxVal;

    // 1. Map colors via linearization table if it exists and the data is not already float
    if (const auto linTable = tiffGetSpan<uint16_t>(tif, TIFFTAG_LINEARIZATIONTABLE); !linTable.empty()) {
        tlog::debug() << fmt::format("Found linearization table of size {}; applying...", linTable.size());

        if (dataSampleFormat == SAMPLEFORMAT_IEEEFP) {
            tlog::warning() << "Data is already in floating point format, but a linearization table is present. Skipping.";
        } else {
            const size_t maxIdx = linTable.size() - 1;

            co_await ThreadPool::global().parallelForAsync<size_t>(
                0,
                numPixels,
                numPixels * numChannels,
                [&](size_t i) {
                    for (size_t c = 0; c < numChannels; ++c) {
                        const float val = rgbaView[c, i];

                        // Lerp the transfer function
                        const size_t idx = clamp((size_t)(val * maxVal), (size_t)0, maxIdx - 1);
                        const float w = val * maxIdx - idx;
                        rgbaView[c, i] = (1.0f - w) * linTable[idx] * scale + w * linTable[idx + 1] * scale;
                    }
                },
                priority
            );
        }
    }

    // 2. Subtract black level
    vector<float> maxBlackLevel(numChannels, 0.0f);
    if (const auto blackLevelFloat = tiffGetSpan<float>(tif, TIFFTAG_BLACKLEVEL); !blackLevelFloat.empty()) {
        uint16_t blackLevelRepeatRows = 1;
        uint16_t blackLevelRepeatCols = 1;
        if (const auto blackLevelRepeatDim = tiffGetSpan<uint16_t>(tif, TIFFTAG_BLACKLEVELREPEATDIM); blackLevelRepeatDim.size() >= 2) {
            blackLevelRepeatRows = blackLevelRepeatDim[0];
            blackLevelRepeatCols = blackLevelRepeatDim[1];
        }

        const size_t numBlackLevelPixels = blackLevelRepeatRows * blackLevelRepeatCols;
        if (numBlackLevelPixels == 0) {
            throw ImageLoadError{"Black level repeat dimensions must not be zero."};
        }

        vector<float> blackLevel(numBlackLevelPixels * numChannels, 0.0f);

        if (blackLevelFloat.size() < blackLevel.size()) {
            throw ImageLoadError{
                fmt::format("Not enough black level data: expected at least {}, got {}", blackLevel.size(), blackLevelFloat.size())
            };
        }

        for (size_t i = 0; i < blackLevel.size(); ++i) {
            blackLevel[i] = blackLevelFloat[i] * scale;
            tlog::debug() << fmt::format("Black level[{}] = {}", i, blackLevel[i]);
        }

        tlog::debug() << fmt::format("Found {}x{} black level data; applying...", blackLevelRepeatRows, blackLevelRepeatCols);

        vector<float> blackLevelDeltaH(size.x(), 0.0f);
        vector<float> blackLevelDeltaV(size.y(), 0.0f);

        if (const auto bldh = tiffGetSpan<float>(tif, TIFFTAG_BLACKLEVELDELTAH); !bldh.empty()) {
            tlog::debug() << fmt::format("Found {} black level H entries", bldh.size());
            if (bldh.size() != blackLevelDeltaH.size()) {
                throw ImageLoadError{"Invalid number of black level delta H pixels."};
            }

            for (size_t i = 0; i < blackLevelDeltaH.size(); ++i) {
                blackLevelDeltaH[i] = bldh[i] * scale;
            }
        }

        if (const auto bldv = tiffGetSpan<float>(tif, TIFFTAG_BLACKLEVELDELTAV); !bldv.empty()) {
            tlog::debug() << fmt::format("Found {} black level V entries", bldv.size());
            if (bldv.size() != blackLevelDeltaV.size()) {
                throw ImageLoadError{"Invalid number of black level delta V pixels."};
            }

            for (size_t i = 0; i < blackLevelDeltaV.size(); ++i) {
                blackLevelDeltaV[i] = bldv[i] * scale;
            }
        }

        vector<float> maxBlackLevelY(numChannels * size.y(), 0.0f);
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels * numChannels,
            [&](const int y) {
                const float deltaV = blackLevelDeltaV[y];
                for (int x = 0; x < size.x(); ++x) {
                    const float deltaH = blackLevelDeltaH[x];
                    const float delta = deltaH + deltaV;
                    const size_t blIdx = (y % blackLevelRepeatRows) * blackLevelRepeatCols + (x % blackLevelRepeatCols);

                    for (size_t c = 0; c < numChannels; ++c) {
                        const float bl = blackLevel[blIdx * numChannels + c] + delta;
                        rgbaView[c, x, y] -= bl;
                        maxBlackLevelY[y * numChannels + c] = std::max(maxBlackLevelY[y * numChannels + c], bl);
                    }
                }
            },
            priority
        );

        maxBlackLevel.assign(numChannels, numeric_limits<float>::lowest());
        for (int y = 0; y < size.y(); ++y) {
            for (size_t c = 0; c < numChannels; ++c) {
                maxBlackLevel[c] = std::max(maxBlackLevel[c], maxBlackLevelY[y * numChannels + c]);
            }
        }
    }

    // 3. Rescale to 0-1 range per white level
    vector<float> whiteLevel(numChannels, 1.0f);
    if (const auto whiteLevelLong = tiffGetSpan<uint32_t>(tif, TIFFTAG_WHITELEVEL); !whiteLevelLong.empty()) {
        if (whiteLevelLong.size() != whiteLevel.size()) {
            throw ImageLoadError{
                fmt::format("Invalid number of long white level pixels: expected {}, got {}", whiteLevel.size(), whiteLevelLong.size())
            };
        }

        tlog::debug() << "Found white level data";

        for (size_t i = 0; i < whiteLevel.size(); ++i) {
            whiteLevel[i] = whiteLevelLong[i] * scale;
        }
    }

    vector<float> channelScale(numChannels);
    for (size_t c = 0; c < numChannels; ++c) {
        tlog::debug() << fmt::format("whiteLevel[{}] = {}", c, whiteLevel[c]);
        tlog::debug() << fmt::format("maxBlackLevel[{}] = {}", c, maxBlackLevel[c]);
        channelScale[c] = 1.0f / (whiteLevel[c] - maxBlackLevel[c]);
    }

    if (any_of(channelScale.begin(), channelScale.end(), [](float s) { return s != 1.0f; })) {
        tlog::debug() << fmt::format("Non-1.0 channel scale [{}]", join(channelScale, ","));

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numChannels; ++c) {
                    rgbaView[c, i] *= channelScale[c];
                }
            },
            priority
        );
    }

    // 4. Clipping: the docs recommend clipping to 1 from above but to keep sub-zero values intact. We will, however, completely skip
    // clipping just in case there's HDR data in there. Per DNG 1.7, this can be the case, so we err on the safe side.
    const bool clipToOne = false;
    if (clipToOne) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numChannels; ++c) {
                    rgbaView[c, i] = std::min(rgbaView[c, i], 1.0f);
                }
            },
            priority
        );
    }
}

Task<void> postprocessLinearRawDng(
    TIFF* tif, const MultiChannelView<float>& rgbView, ImageData& resultData, const bool reverseEndian, const bool applyCameraProfile, const int priority
) {
    const size_t numChannels = rgbView.nChannels();
    if (numChannels != 3) {
        throw ImageLoadError{"Linear RAW image with samplesPerPixel != 3 are not supported."};
    }

    // We follow page 96 of https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_0_0.pdf
    tlog::debug() << "Mapping LinearRAW to linear RGB...";

    const auto size = resultData.size();
    const auto numPixels = (size_t)size.x() * size.y();

    // Camera parameters are stored in IFD 0, so let's switch to it temporarily.
    const uint64_t prevOffset = TIFFCurrentDirOffset(tif);
    TIFFSetDirectory(tif, 0);
    const ScopeGuard guard{[&]() { TIFFSetSubDirectory(tif, prevOffset); }};

    Vector3f analogBalance{1.0f};
    if (const auto abt = tiffGetSpan<float>(tif, TIFFTAG_ANALOGBALANCE); !abt.empty()) {
        if (abt.size() != numChannels) {
            throw ImageLoadError{"Invalid number of analog balance pixels."};
        }

        for (size_t i = 0; i < abt.size(); ++i) {
            analogBalance[i] = abt[i];
        }

        tlog::debug() << fmt::format("Analog balance: {}", analogBalance);
    }

    const auto readCameraToXyz = [&](int CCTAG, int CMTAG, int CALTAG) {
        Matrix3f colorMatrix{1.0f};
        if (const auto cmt = tiffGetSpan<float>(tif, CMTAG); !cmt.empty()) {
            if (cmt.size() != numChannels * numChannels) {
                throw ImageLoadError{"Invalid number of camera matrix entries."};
            }

            for (size_t i = 0; i < numChannels; ++i) {
                for (size_t j = 0; j < numChannels; ++j) {
                    colorMatrix.m[j][i] = cmt[i * numChannels + j];
                }
            }

            tlog::debug() << fmt::format("Found color matrix: {}", colorMatrix);
        } else {
            return optional<Matrix3f>{};
        }

        Matrix3f cameraCalibration{1.0f};
        if (const auto cct = tiffGetSpan<float>(tif, CCTAG); !cct.empty()) {
            if (cct.size() != numChannels * numChannels) {
                throw ImageLoadError{"Invalid number of camera calibration entries."};
            }

            for (size_t i = 0; i < numChannels; ++i) {
                for (size_t j = 0; j < numChannels; ++j) {
                    cameraCalibration.m[j][i] = cct[i * numChannels + j];
                }
            }

            tlog::debug() << fmt::format("Found camera calibration matrix: {}", cameraCalibration);
        }

        Matrix3f chromaticAdaptation{1.0f};

        // From preliminary tests, it seems that the color matrix from the DNG file does not need to be adapted to the exif illuminant.
        // Still, we leave that as an option here.
        const bool adaptToExifIlluminant = false;

        const auto xyzToCamera = Matrix3f::scale(analogBalance) * cameraCalibration * colorMatrix;
        const auto cameraToXyz = inverse(xyzToCamera);

        // Read AsShotNeutral (camera's response to the scene white point, normalized to green=1)
        if (const auto asn = tiffGetSpan<float>(tif, TIFFTAG_ASSHOTNEUTRAL); !asn.empty()) {
            if (asn.size() != numChannels) {
                throw ImageLoadError{"Invalid number of AsShotNeutral entries."};
            }

            const Vector3f asShotNeutral{asn[0], asn[1], asn[2]};
            tlog::debug() << fmt::format("Adapting white to D50 based on AsShotNeutral={}", asShotNeutral);

            const Vector3f xyz = cameraToXyz * asShotNeutral;
            const float sxyz = xyz.x() + xyz.y() + xyz.z();
            const Vector2f xy = {xyz.x() / sxyz, xyz.y() / sxyz};

            chromaticAdaptation = adaptWhiteBradford(xy, whiteD50());
        } else if (const auto aswp = tiffGetSpan<float>(tif, TIFFTAG_ASSHOTWHITEXY); !aswp.empty()) {
            if (aswp.size() != 2) {
                throw ImageLoadError{"Invalid number of AsShotNeutral entries."};
            }

            const Vector2f cameraNeutralXy = {aswp[0], aswp[1]};

            tlog::debug() << fmt::format("Adapting white to D50 based on AsShotWhiteXY={}", cameraNeutralXy);
            chromaticAdaptation = adaptWhiteBradford(cameraNeutralXy, whiteD50());
        } else if (const auto illu = tiffGetValue<uint16_t>(tif, CALTAG); illu && adaptToExifIlluminant) {
            EExifLightSource illuminant = static_cast<EExifLightSource>(*illu);
            tlog::debug() << fmt::format("Found illuminant={}/{}", toString(illuminant), *illu);

            const auto whitePoint = xy(illuminant);
            if (whitePoint.x() > 0.0f && whitePoint.y() > 0.0f) {
                tlog::debug() << fmt::format("Adapting known illuminant with CIE1931 xy={} to D50", whitePoint);
                chromaticAdaptation = adaptWhiteBradford(whitePoint, whiteD50());
            } else {
                tlog::warning() << fmt::format("Unknown illuminant");
            }
        }

        return optional<Matrix3f>{chromaticAdaptation * cameraToXyz};
    };

    const array<tuple<uint16_t, uint16_t, uint16_t>, 3> camTags = {
        {
         {TIFFTAG_CAMERACALIBRATION3, TIFFTAG_COLORMATRIX3, TIFFTAG_CALIBRATIONILLUMINANT3},
         {TIFFTAG_CAMERACALIBRATION2, TIFFTAG_COLORMATRIX2, TIFFTAG_CALIBRATIONILLUMINANT2},
         {TIFFTAG_CAMERACALIBRATION1, TIFFTAG_COLORMATRIX1, TIFFTAG_CALIBRATIONILLUMINANT1},
         }
    };

    resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;
    resultData.toRec709 = xyzToChromaMatrix(rec709Chroma()) * adaptWhiteBradford(whiteD50(), whiteD65());

    // If present, matrix 3 represents the illuminant used to capture the image. If not, we use the illuminant from matrix 2 which
    // is supposed to be the colder one (closer to D65).
    auto toRimm = xyzToChromaMatrix(proPhotoChroma());
    for (size_t i = 0; i < camTags.size(); ++i) {
        if (const auto camToXyz = readCameraToXyz(get<0>(camTags[i]), get<1>(camTags[i]), get<2>(camTags[i]))) {
            tlog::debug() << fmt::format("Applying camToXyz matrix #{}: {}", camTags.size() - i, camToXyz.value());
            resultData.toRec709 = resultData.toRec709 * camToXyz.value();
            toRimm = toRimm * camToXyz.value();
            break;
        }
    }

    struct ProfileDynamicRange {
        uint16_t version, dynamicRange;
        float hintMaxOutputValue;
    };

    // NOTE: The order of the following operations is defined on pages 71/72 of the DNG spec.
    float exposureScale = 1.0f;
    exposureScale *= exp2f(tiffGetValue<float>(tif, TIFFTAG_BASELINEEXPOSURE).value_or(0.0f));
    exposureScale *= exp2f(tiffGetValue<float>(tif, TIFFTAG_BASELINEEXPOSUREOFFSET).value_or(0.0f));

    if (exposureScale != 1.0f) {
        tlog::debug() << fmt::format("Applying exposure scale: {}", exposureScale);
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numChannels; ++c) {
                    rgbView[c, i] *= exposureScale;
                }
            },
            priority
        );
    }

    // At this point, we have the image in a linear scale, with known conversion to xyz (and thus to rec709) for tev to display. This is
    // most faithful to the readings from the sensor *in theory*, but the camera may have embedded a (potentially user-chosen) color profile
    // that, per the DNG spec, can be used as a starting point for further user editing. *In practice*, DNGs from some sources, e.g. iPhone,
    // look like cleaner (less washed out, but also less dynamic range) when the profile is applied, so it's a judgement call whether to
    // apply it or not.

    if (!applyCameraProfile) {
        co_return;
    }

    // The remaining camera profile transformation is applied in linear ProPhoto RGB space (aka RIMM space)
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * numChannels,
        [&](size_t i) {
            auto rgb = Vector3f{rgbView[0, i], rgbView[1, i], rgbView[2, i]};
            rgb = toRimm * rgb;
            rgbView[0, i] = rgb.x();
            rgbView[1, i] = rgb.y();
            rgbView[2, i] = rgb.z();
        },
        priority
    );

    resultData.toRec709 = resultData.toRec709 * inverse(toRimm);

    bool isHdr = false;
    static constexpr uint16_t TIFFTAG_PROFILEDYNAMICRANGE = 52551;
    if (const auto pdrData = tiffGetSpan<uint8_t>(tif, TIFFTAG_PROFILEDYNAMICRANGE); pdrData.size() >= sizeof(ProfileDynamicRange)) {
        ProfileDynamicRange pdr = fromBytes<ProfileDynamicRange>(pdrData);
        if (reverseEndian) {
            pdr.version = swapBytes(pdr.version);
            pdr.dynamicRange = swapBytes(pdr.dynamicRange);
            pdr.hintMaxOutputValue = swapBytes(pdr.hintMaxOutputValue);
        }

        tlog::debug() << fmt::format(
            "Found profile dynamic range: version={} dynamicRange={} hintMaxOutputValue={}", pdr.version, pdr.dynamicRange, pdr.hintMaxOutputValue
        );

        // Per DNG 1.7.0.0, page 93, a value of 1 refers to HDR images that need to be compressed into 0-1 before the following transforms
        // take place.
        isHdr = pdr.dynamicRange == 1;
    }

    if (const auto str = tiffGetSpan<char>(tif, TIFFTAG_PROFILENAME); !str.empty()) {
        tlog::debug() << fmt::format("Applying camera profile \"{}\"", str.data());
    }

    {
        // Temporarily switch back to the raw's IFD to read gain table map, if present.
        TIFFSetSubDirectory(tif, prevOffset);
        ScopeGuard guard2{[&]() { TIFFSetDirectory(tif, 0); }};

        // TODO: support TIFFTAG_PROFILEGAINTABLEMAP2

        if (const auto gainTableMap = tiffGetSpan<uint8_t>(tif, TIFFTAG_PROFILEGAINTABLEMAP); !gainTableMap.empty()) {
            struct GainTableMapHeader {
                uint32_t mapPointsV, mapPointsH;
                double mapSpacingV, mapSpacingH;
                double mapOriginV, mapOriginH;
                uint32_t mapPointsN;
                array<float, 5> mapInputWeights;
            };

            static_assert(sizeof(GainTableMapHeader) == 64, "Unexpected padding in GainTableMapHeader");

            if (gainTableMap.size() < sizeof(GainTableMapHeader)) {
                throw ImageLoadError{fmt::format(
                    "Gain table map is too small to contain header: expected at least {}, got {}",
                    sizeof(GainTableMapHeader),
                    gainTableMap.size()
                )};
            }

            GainTableMapHeader header = fromBytes<GainTableMapHeader>(gainTableMap);
            if (reverseEndian) {
                header.mapPointsV = swapBytes(header.mapPointsV);
                header.mapPointsH = swapBytes(header.mapPointsH);
                header.mapPointsN = swapBytes(header.mapPointsN);
                header.mapSpacingV = swapBytes(header.mapSpacingV);
                header.mapSpacingH = swapBytes(header.mapSpacingH);
                header.mapOriginV = swapBytes(header.mapOriginV);
                header.mapOriginH = swapBytes(header.mapOriginH);
                for (float& w : header.mapInputWeights) {
                    w = swapBytes(w);
                }
            }

            const auto numValues = (size_t)header.mapPointsV * (size_t)header.mapPointsH * header.mapPointsN;
            if (numValues == 0) {
                throw ImageLoadError{"Gain table map must have non-zero points in all dimensions."};
            }

            const auto numBytes = sizeof(GainTableMapHeader) + numValues * sizeof(float);
            if (gainTableMap.size() < numBytes) {
                throw ImageLoadError{
                    fmt::format("Gain table map is too small to contain values: expected at least {}, got {}", numBytes, gainTableMap.size())
                };
            }

            const span<const float> valueSpan{(const float*)(gainTableMap.data() + sizeof(GainTableMapHeader)), numValues};
            vector<float> values(valueSpan.begin(), valueSpan.end());
            if (reverseEndian) {
                for (float& v : values) {
                    v = swapBytes(v);
                }
            }

            tlog::debug() << fmt::format(
                "Found gain table map: points={}x{}x{} spacing=[{:.4f}, {:.4f}] origin=[{:.4f}, {:.4f}] inputWeights={}",
                header.mapPointsV,
                header.mapPointsH,
                header.mapPointsN,
                header.mapSpacingV,
                header.mapSpacingH,
                header.mapOriginV,
                header.mapOriginH,
                header.mapInputWeights
            );

            const Vector2f invSize = Vector2f{1.0f} / Vector2f{size};
            const Vector3f invMapSpacing = Vector3f{1.0f / (float)header.mapSpacingH, 1.0f / (float)header.mapSpacingV, 255.0f};

            const Vector3i maxIdx = Vector3i{(int)header.mapPointsH - 1, (int)header.mapPointsV - 1, (int)header.mapPointsN - 1};
            const auto sample = [&](int ix, int iy, int iz) -> float {
                return values[(size_t)iy * header.mapPointsH * header.mapPointsN + (size_t)ix * header.mapPointsN + iz];
            };

            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                numPixels * numChannels,
                [&](int y) {
                    const size_t offset = (size_t)y * size.x();
                    for (int x = 0; x < size.x(); ++x) {
                        const size_t i = offset + x;

                        // Dot product of (R, G, B, minRGB, maxRGB) and mapInputWeights, clamped to [0, 1]. This will index into mapPointsN.
                        float input = 0.0f, maxRgb = -numeric_limits<float>::infinity(), minRgb = numeric_limits<float>::infinity();
                        for (size_t c = 0; c < 3; ++c) {
                            const float v = rgbView[c, i];
                            input += v * header.mapInputWeights[c];
                            maxRgb = std::max(maxRgb, v);
                            minRgb = std::min(minRgb, v);
                        }

                        input = std::clamp(input + header.mapInputWeights[3] * minRgb + header.mapInputWeights[4] * maxRgb, 0.0f, 1.0f);

                        const Vector2f relXy = (Vector2f{(float)x, (float)y} + 0.5f) * invSize;
                        const Vector3f mapXyz = Vector3f{relXy.x() - (float)header.mapOriginH, relXy.y() - (float)header.mapOriginV, input} *
                            invMapSpacing;

                        const Vector3f clamped = min(max(mapXyz, Vector3f{0.0f}), Vector3f{maxIdx});

                        const Vector3i p0 = min(Vector3i{clamped}, max(maxIdx - 1, Vector3i{0}));
                        const Vector3i p1 = min(p0 + Vector3i{1}, maxIdx);

                        // Clamped to [0, 1] to make out-of-range values use the gain from the closest valid point
                        const Vector3f f = min(max(clamped - Vector3f{p0}, Vector3f{0.0f}), Vector3f{1.0f});

                        const float c000 = sample(p0.x(), p0.y(), p0.z());
                        const float c001 = sample(p0.x(), p0.y(), p1.z());
                        const float c010 = sample(p0.x(), p1.y(), p0.z());
                        const float c011 = sample(p0.x(), p1.y(), p1.z());
                        const float c100 = sample(p1.x(), p0.y(), p0.z());
                        const float c101 = sample(p1.x(), p0.y(), p1.z());
                        const float c110 = sample(p1.x(), p1.y(), p0.z());
                        const float c111 = sample(p1.x(), p1.y(), p1.z());

                        const float c00 = c000 * (1.0f - f.z()) + c001 * f.z();
                        const float c01 = c010 * (1.0f - f.z()) + c011 * f.z();
                        const float c10 = c100 * (1.0f - f.z()) + c101 * f.z();
                        const float c11 = c110 * (1.0f - f.z()) + c111 * f.z();

                        const float c0 = c00 * (1.0f - f.y()) + c01 * f.y();
                        const float c1 = c10 * (1.0f - f.y()) + c11 * f.y();

                        const float gain = c0 * (1.0f - f.x()) + c1 * f.x();

                        for (size_t c = 0; c < numChannels; ++c) {
                            rgbView[c, i] *= gain;
                        }
                    }
                },
                priority
            );
        }
    }

    // Profile application has to happen in SDR space if the image is HDR
    if (isHdr) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numChannels; ++c) {
                    float& v = rgbView[c, i];
                    v = dngHdrEncodingFunction(v);
                }
            },
            priority
        );
    }

    if (const auto dims = tiffGetSpan<uint32_t>(tif, TIFFTAG_PROFILEHUESATMAPDIMS); dims.size() >= 3) {
        uint32_t hueDivisions = dims[0];
        uint32_t satDivisions = dims[1];
        uint32_t valueDivisions = dims[2];

        tlog::debug() << fmt::format("Hue/sat/val map dimensions: {}x{}x{}", hueDivisions, satDivisions, valueDivisions);

        // TODO: implement hue/sat/val map...
        tlog::debug() << "Found hue/sat/val map, but not implemented yet. Color profile may look wrong.";
    }

    if (const auto dims = tiffGetSpan<uint32_t>(tif, TIFFTAG_PROFILELOOKTABLEDIMS); dims.size() >= 3) {
        uint32_t hueDivisions = dims[0];
        uint32_t satDivisions = dims[1];
        uint32_t valueDivisions = dims[2];

        tlog::debug() << fmt::format("Look table dimensions: {}x{}x{}", hueDivisions, satDivisions, valueDivisions);

        // TODO: implement hue/sat/val map...
        tlog::debug() << "Found look table, but not implemented yet. Color profile may look wrong.";
    }

    if (const auto tonecurve = tiffGetSpan<float>(tif, TIFFTAG_PROFILETONECURVE); !tonecurve.empty()) {
        if (tonecurve.size() % 2 != 0 || tonecurve.size() < 4) {
            throw ImageLoadError{"Number of tone curve entries must be divisible by 2 and >=4."};
        }

        tlog::debug() << fmt::format("Applying profile tone curve of length {}", tonecurve.size());

        const auto tc = tonecurve | fixed_chunks<2> | views::transform([](auto c) { return Vector2f{c[0], c[1]}; }) | to_vector;
        if (tc.front().x() != 0.0f || tc.back().x() != 1.0f) {
            throw ImageLoadError{"Tone curve must start at 0."};
        }

        const auto applyPwLinear = [](span<const Vector2f> tc, float x) {
            const auto it = lower_bound(tc.begin(), tc.end(), x, [](auto a, float b) { return a.x() < b; });

            // The spec says to extend the slope of the last segment.
            const int i = clamp((int)distance(tc.begin(), it) - 1, 0, (int)tc.size() - 2);

            // TODO: Docs say to use cubic spline interpolation, whereas we're using linear interpolation. The difference seems to
            // be negligible so far, but we should fix this at some point.
            const float w = (x - tc[i].x()) / (tc[i + 1].x() - tc[i].x());
            return (1.0f - w) * tc[i].y() + w * tc[i + 1].y();
        };

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numChannels * 16, // arbitrary factor to estimate pw linear cost
            [&](size_t i) {
                for (size_t c = 0; c < numChannels; ++c) {
                    float& v = rgbView[c, i];
                    v = applyPwLinear(tc, v);
                }
            },
            priority
        );
    }

    if (TIFFFindField(tif, TIFFTAG_RGBTABLES, TIFFDataType::TIFF_ANY)) {
        tlog::warning() << "Found RGB tables, but not implemented yet. Color profile may look wrong.";
    }

    if (isHdr) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numChannels; ++c) {
                    float& v = rgbView[c, i];
                    v = dngHdrDecodingFunction(v);
                }
            },
            priority
        );
    }
}

Task<void> postprocessRgb(
    TIFF* tif,
    const uint16_t photometric,
    const uint16_t dataBitsPerSample,
    const size_t numColorChannels,
    const MultiChannelView<float>& rgbaView,
    ImageData& resultData,
    const int priority
) {
    if (numColorChannels < rgbaView.nChannels()) {
        throw ImageLoadError{"Not enough color channels in the image."};
    }

    const Vector2i size = resultData.size();
    const size_t numPixels = (size_t)size.x() * size.y();

    const size_t bps = photometric == PHOTOMETRIC_PALETTE ? 16 : dataBitsPerSample;

    if (const auto referenceBw = tiffGetSpan<float>(tif, TIFFTAG_REFERENCEBLACKWHITE); referenceBw.size() >= 6) {
        const size_t maxVal = (1ull << bps) - 1;

        const bool isYCbCr = photometric == PHOTOMETRIC_YCBCR;
        const Vector3f codingRange = {(float)maxVal, isYCbCr ? 127.0f : (float)maxVal, isYCbCr ? 127.0f : (float)maxVal};

        const Vector3f refBlack = Vector3f{referenceBw[0], referenceBw[2], referenceBw[4]};
        const Vector3f refWhite = Vector3f{referenceBw[1], referenceBw[3], referenceBw[5]};
        const Vector3f invRange = 1.0f / (refWhite - refBlack);

        const Vector3f offset = isYCbCr ? Vector3f{0.0f, 0.5f, 0.5f} : Vector3f{0.0f};

        const Vector3f totalScale = codingRange * invRange / maxVal;

        tlog::debug() << fmt::format("Found reference black/white: black={} white={}", refBlack, refWhite);

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numColorChannels; ++c) {
                    rgbaView[c, i] = (rgbaView[c, i] * maxVal - refBlack[c]) * totalScale[c] + offset[c];
                }
            },
            priority
        );
    }

    if (photometric == PHOTOMETRIC_YCBCR && numColorChannels >= 3) {
        Vector4f coeffs = {1.402f, -0.344136f, -0.714136f, 1.772f};
        if (const auto yCbCrCoeffs = tiffGetSpan<float>(tif, TIFFTAG_YCBCRCOEFFICIENTS); yCbCrCoeffs.size() >= 3) {
            const Vector3f K = {yCbCrCoeffs[0], yCbCrCoeffs[1], yCbCrCoeffs[2]};
            coeffs = {
                2.0f * (1.0f - K.x()),
                -2.0f * K.z() * (1.0f - K.z()) / K.y(),
                -2.0f * K.x() * (1.0f - K.x()) / K.y(),
                2.0f * (1.0f - K.z()),
            };

            tlog::debug() << fmt::format("Found YCbCr coefficients: {} -> {}", K, coeffs);
        }

        co_await yCbCrToRgb(rgbaView, priority, coeffs);
    }

    chroma_t chroma = rec709Chroma();
    if (const auto primaries = tiffGetSpan<float>(tif, TIFFTAG_PRIMARYCHROMATICITIES); primaries.size() >= 6) {
        tlog::debug() << "Found custom primaries; applying...";
        chroma[0] = {primaries[0], primaries[1]};
        chroma[1] = {primaries[2], primaries[3]};
        chroma[2] = {primaries[4], primaries[5]};
    }

    if (const auto whitePoint = tiffGetSpan<float>(tif, TIFFTAG_WHITEPOINT); whitePoint.size() >= 2) {
        tlog::debug() << "Found custom white point; applying...";
        chroma[3] = {whitePoint[0], whitePoint[1]};
    }

    // Assume the RGB TIFF image is display-referred and not scene-referred, so we'll adapt the white point. While scene-referred linear
    // TIFF images *do* exist in the wild, there is, unfortunately, no unambiguous way to determine this from the TIFF metadata alone.
    resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;
    resultData.toRec709 = convertColorspaceMatrix(chroma, rec709Chroma(), resultData.renderingIntent);
    resultData.nativeMetadata.chroma = chroma;

    enum EPreviewColorSpace : uint32_t { Unknown = 0, Gamma2_2 = 1, sRGB = 2, AdobeRGB = 3, ProPhotoRGB = 4 };

    if (const auto transferFunction = tiffGetTransferFunction(tif); !transferFunction.at(0).empty()) {
        // In TIFF, transfer functions are stored as 2**bitsPerSample values in the range [0, 65535] per color channel. The transfer
        // function is a linear interpolation between these values.
        tlog::debug() << "Found custom transfer function; applying...";

        if (transferFunction.size() < numColorChannels || numColorChannels > 3) {
            throw ImageLoadError{"TIFF images with transfer functions and more than 3 color channels are not supported."};
        }

        for (size_t c = 0; c < numColorChannels; ++c) {
            if (transferFunction.at(c).size() < 2) {
                throw ImageLoadError{fmt::format("Missing transfer function for channel {}", c)};
            }
        }

        const size_t maxIdx = (1ull << bps) - 1;

        Vector3i transferRangeBlack = {0};
        Vector3i transferRangeWhite = {65535};

        static constexpr uint16_t TIFFTAG_TRANSFERRANGE = 342;

        if (const auto transferRange = tiffGetSpan<uint16_t>(tif, TIFFTAG_TRANSFERRANGE); transferRange.size() >= 6) {
            transferRangeBlack = {transferRange[0], transferRange[2], transferRange[4]};
            transferRangeWhite = {transferRange[1], transferRange[3], transferRange[5]};

            tlog::debug() << fmt::format("Found transfer range [{}, {}]", transferRangeBlack, transferRangeWhite);
        }

        const Vector3f scale = Vector3f(1.0f) / Vector3f(transferRangeWhite - transferRangeBlack);

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numColorChannels; ++c) {
                    const float val = rgbaView[c, i];

                    // Lerp the transfer function
                    const size_t idx = clamp((size_t)(val * maxIdx) + transferRangeBlack[c], (size_t)0, transferFunction[c].size() - 2);
                    const float w = val * maxIdx - idx - transferRangeBlack[c];
                    rgbaView[c, i] = ((1.0f - w) * (float)transferFunction[c][idx] + w * (float)transferFunction[c][idx + 1] -
                                      transferRangeBlack[c]) *
                        scale[c];
                }
            },
            priority
        );

        resultData.nativeMetadata.transfer = ituth273::ETransfer::LUT;
    } else if (const auto pcsInt = tiffGetValue<uint32_t>(tif, TIFFTAG_PREVIEWCOLORSPACE)) {
        // Alternatively, if we're a preview image from a DNG file, we can use the preview color space to determine the transfer. Values
        // 0 (Unknown) and 1 (Gamma 2.2) are handled by the following `else` block. Other values are handled in this one.
        tlog::debug() << fmt::format("Found preview color space: {}", *pcsInt);

        const EPreviewColorSpace pcs = static_cast<EPreviewColorSpace>(*pcsInt);

        size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numColorChannels; ++c) {
                    float& v = rgbaView[c, i];
                    v = toLinear(v);
                }
            },
            priority
        );

        resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;

        if (pcs == EPreviewColorSpace::AdobeRGB) {
            const auto chroma = adobeChroma();
            resultData.toRec709 = convertColorspaceMatrix(chroma, rec709Chroma(), resultData.renderingIntent);
            resultData.nativeMetadata.chroma = chroma;
        } else if (pcs == EPreviewColorSpace::ProPhotoRGB) {
            const auto chroma = adobeChroma();
            resultData.toRec709 = convertColorspaceMatrix(proPhotoChroma(), rec709Chroma(), resultData.renderingIntent);
            resultData.nativeMetadata.chroma = chroma;
        }
    } else {
        // If there's no transfer function specified, the TIFF spec says to use gamma 2.2 for RGB data and no transfer (linear) for
        // grayscale data. That said, all grayscale TIFF images I've seen in the wild so far assume gamma 2.2, so we'll go against the
        // spec here.
        tlog::debug() << "No transfer function found; assuming gamma 2.2 for RGB data per the TIFF spec.";

        size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numColorChannels; ++c) {
                    // We use the absolute value here to avoid having to clamp negative values to 0 -- we instead pretend that
                    // the power behaves like an odd exponent, thereby preserving the range of R.
                    float& v = rgbaView[c, i];
                    v = copysign(pow(abs(v), 2.2f), v);
                }
            },
            priority
        );

        resultData.nativeMetadata.transfer = ituth273::ETransfer::Gamma22;
    }
}

Task<void> postprocessLab(
    TIFF* tif,
    const uint16_t photometric,
    const uint16_t dataBitsPerSample,
    const size_t numColorChannels,
    const MultiChannelView<float>& rgbaView,
    ImageData& resultData,
    const int priority
) {
    if (numColorChannels != 3) {
        throw ImageLoadError{"CIELAB images without 3 color channels are not supported."};
    }

    const Vector2i size = resultData.size();
    const size_t numPixels = (size_t)size.x() * size.y();

    // Step 1: Decode the encoded values to CIE L*a*b* [L: 0..100, a: -128..127, b: -128..127]
    if (photometric == PHOTOMETRIC_CIELAB) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                rgbaView[0, i] *= 100.0f;
                for (size_t c = 1; c < numColorChannels; ++c) {
                    float& v = rgbaView[c, i];
                    v *= 255.0f;
                    v = v >= 128.0f ? v - 256.0f : v;
                }
            },
            priority
        );
    } else if (photometric == PHOTOMETRIC_ICCLAB) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                rgbaView[0, i] *= 100.0f;
                for (size_t c = 1; c < numColorChannels; ++c) {
                    float& v = rgbaView[c, i];
                    v = v * 255.0f - 128.0f;
                }
            },
            priority
        );
    } else if (photometric == PHOTOMETRIC_ITULAB) {
        Vector3f decodeMin = {0.0f, -85.0f, -85.0f}, decodeMax = {100.0f, 85.0f, 85.0f};
        if (const auto decode = tiffGetSpan<float>(tif, TIFFTAG_DECODE); decode.size() >= 6) {
            decodeMin = {decode[0], decode[2], decode[4]};
            decodeMax = {decode[1], decode[3], decode[5]};

            tlog::debug() << fmt::format("Found ITULAB Decode tag: min={} max={}", decodeMin, decodeMax);
        }

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (size_t c = 0; c < numColorChannels; ++c) {
                    float& v = rgbaView[c, i];
                    v = decodeMin[c] + v * (decodeMax[c] - decodeMin[c]);
                }
            },
            priority
        );
    }

    // Step 2: Convert CIE L*a*b* to CIE XYZ. We can then convert from XYZ to linear sRGB/Rec709 using a simple matrix transform
    const Vector3f whitePointXYZ = {0.9642f, 1.0f, 0.8249f}; // D50

    static constexpr float kappa = 903.3f; // 24389/27
    static constexpr float epsilon = 0.008856f; // 216/24389

    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * numColorChannels,
        [&](size_t i) {
            const float L = rgbaView[0, i];
            const float a = rgbaView[1, i];
            const float b = rgbaView[2, i];

            const float fy = (L + 16.0f) / 116.0f;
            const float fx = a / 500.0f + fy;
            const float fz = fy - b / 200.0f;

            const float fx3 = fx * fx * fx;
            const float fz3 = fz * fz * fz;

            const float xr = (fx3 > epsilon) ? fx3 : (116.0f * fx - 16.0f) / kappa;
            const float yr = (L > kappa * epsilon) ? ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f) : L / kappa;
            const float zr = (fz3 > epsilon) ? fz3 : (116.0f * fz - 16.0f) / kappa;

            const float X = xr * whitePointXYZ.x();
            const float Y = yr * whitePointXYZ.y();
            const float Z = zr * whitePointXYZ.z();

            rgbaView[0, i] = X;
            rgbaView[1, i] = Y;
            rgbaView[2, i] = Z;
        },
        priority
    );

    resultData.renderingIntent = ERenderingIntent::AbsoluteColorimetric;
    resultData.toRec709 = xyzToChromaMatrix(rec709Chroma()) * adaptWhiteBradford(whiteD50(), whiteD65());

    co_return;
}

Task<ImageData> decodeJpeg(
    span<const uint8_t> compressedData,
    span<const uint8_t> jpegTables,
    const Vector2i& tileSize,
    uint16_t tileNumComponents,
    size_t* nestedBitsPerSample,
    int photometric,
    int priority
) {
    vector<uint8_t> stream;
    if (jpegTables.size() > 4) {
        // tlog::debug() << "JPEG tables found; prepending to compressed data...";

        const uint32_t tablesPayloadLen = jpegTables.size() - 4;
        const uint8_t* tablesPayload = jpegTables.data() + 2;

        stream.resize(2 + tablesPayloadLen + (compressedData.size() - 2));
        memcpy(stream.data(), compressedData.data(), 2);
        memcpy(stream.data() + 2, tablesPayload, tablesPayloadLen);
        memcpy(stream.data() + 2 + tablesPayloadLen, compressedData.data() + 2, compressedData.size() - 2);

        compressedData = stream;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr cinfo) {
        char buf[JMSG_LENGTH_MAX];
        cinfo->err->format_message(cinfo, buf);
        throw ImageLoadError{fmt::format("libjpeg error: {}", buf)};
    };
    jerr.output_message = [](j_common_ptr cinfo) {
        char buf[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message)(cinfo, buf);
        tlog::warning() << fmt::format("libjpeg warning: {}", buf);
    };

    jpeg_create_decompress(&cinfo);
    const ScopeGuard guard{[&]() { jpeg_destroy_decompress(&cinfo); }};

    jpeg_mem_src(&cinfo, compressedData.data(), compressedData.size());

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        throw ImageLoadError{"Failed to read JPEG header."};
    }

    const int precision = cinfo.data_precision;

    if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
        throw ImageLoadError{"CMYK JPEGs are not supported."};
    }

    if (precision < 2 || precision > 16) {
        throw ImageLoadError{fmt::format("Unsupported JPEG precision: {} bits per sample.", precision)};
    }

    const auto pixelFormat = cinfo.data_precision > 8 ? cinfo.data_precision > 12 ? EPixelFormat::U16 : EPixelFormat::I16 : EPixelFormat::U8;

    // Suppress all color conversion; output in the native colorspace. We'll convert outselves.
    cinfo.out_color_space = cinfo.jpeg_color_space;
    cinfo.quantize_colors = false;
    jpeg_start_decompress(&cinfo);
    ScopeGuard decompressGuard{[&]() { jpeg_abort_decompress(&cinfo); }};

    const auto width = (size_t)cinfo.output_width;
    const auto height = (size_t)cinfo.output_height;
    const auto numComponents = (size_t)cinfo.output_components;

    const auto numJpegPixels = (size_t)width * height;
    const auto numTilePixels = (size_t)tileSize.x() * tileSize.y();

    const auto numJpegSamples = numJpegPixels * numComponents;
    const auto numTileSamples = numTilePixels * tileNumComponents;

    if (numJpegSamples < numTileSamples) {
        throw ImageLoadError{fmt::format(
            "Decompressed JPEG has fewer samples ({}) than expected from the tile size and samples per pixel ({}).", numJpegSamples, numTileSamples
        )};
    }

    const float scale = 1.0f / (float)((1ull << precision) - 1);

    ImageData result;
    result.channels = co_await ImageLoader::makeRgbaInterleavedChannels(
        tileNumComponents, tileNumComponents, false, tileSize, EPixelFormat::F32, EPixelFormat::F16, "", priority
    );

    const auto outView = MultiChannelView<float>{result.channels};

    // tlog::debug() << fmt::format(
    //     "Decompressing JPEG with width={} height={} components={} precision={} colorspace={}",
    //     width,
    //     height,
    //     numComponents,
    //     precision,
    //     (int)cinfo.jpeg_color_space
    // );

    auto buf = PixelBuffer::alloc((size_t)width * height * numComponents, pixelFormat);

    if (cinfo.data_precision <= 8) {
        HeapArray<JSAMPROW> rowPointers(height);
        for (size_t y = 0; y < height; ++y) {
            rowPointers[y] = buf.data<uint8_t>() + y * width * numComponents;
        }

        while (cinfo.output_scanline < cinfo.output_height) {
            jpeg_read_scanlines(&cinfo, &rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
        }
    } else if (cinfo.data_precision <= 12) {
        HeapArray<J12SAMPROW> rowPointers(height);
        for (size_t y = 0; y < height; ++y) {
            rowPointers[y] = buf.data<int16_t>() + y * width * numComponents;
        }

        while (cinfo.output_scanline < cinfo.output_height) {
            jpeg12_read_scanlines(&cinfo, &rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
        }
    } else {
        HeapArray<J16SAMPROW> rowPointers(height);
        for (size_t y = 0; y < height; ++y) {
            rowPointers[y] = buf.data<uint16_t>() + y * width * numComponents;
        }

        while (cinfo.output_scanline < cinfo.output_height) {
            jpeg16_read_scanlines(&cinfo, &rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
        }
    }

    if (pixelFormat == EPixelFormat::U8) {
        co_await toFloat32(buf.data<const uint8_t>(), tileNumComponents, outView, false, priority, scale);
    } else if (pixelFormat == EPixelFormat::I16) {
        co_await toFloat32(buf.data<const int16_t>(), tileNumComponents, outView, false, priority, scale);
    } else if (pixelFormat == EPixelFormat::U16) {
        co_await toFloat32(buf.data<const uint16_t>(), tileNumComponents, outView, false, priority, scale);
    } else {
        throw ImageLoadError{fmt::format("Unsupported pixel format: {}", toString(pixelFormat))};
    }

    decompressGuard.disarm();
    jpeg_finish_decompress(&cinfo);

    *nestedBitsPerSample = (size_t)precision;
    co_return result;
}

Task<ImageData> readTiffImage(
    const TiffData& tiffData,
    TIFF* tif,
    const bool isDng,
    const bool reverseEndian,
    string_view partName,
    const ImageLoaderSettings& settings,
    const int priority
) {
    uint32_t width, height;
    if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height)) {
        throw ImageLoadError{"Failed to read dimensions."};
    }

    // Note: libtiff doesn't support variable bits per sample, which is technically allowed by the TIFF 6.0 spec. We assume all samples
    // have the same bit depth.
    uint16_t bitsPerSample;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample)) {
        throw ImageLoadError{"Failed to read bits per sample."};
    }

    uint16_t tiffInternalBitsPerSample = bitsPerSample;

    uint16_t samplesPerPixel;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel)) {
        throw ImageLoadError{"Failed to read samples per pixel."};
    }

    uint16_t sampleFormat;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat)) {
        throw ImageLoadError{"Failed to read sample format."};
    }

    // Interpret untyped data as unsigned integer... let's try displaying it
    if (sampleFormat == SAMPLEFORMAT_VOID) {
        sampleFormat = SAMPLEFORMAT_UINT;
    }

    if (sampleFormat > SAMPLEFORMAT_IEEEFP) {
        throw ImageLoadError{fmt::format("Unsupported sample format: {}", sampleFormat)};
    }

    uint16_t compression;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression)) {
        throw ImageLoadError{"Failed to read compression type."};
    }

    uint16_t photometric;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric)) {
        throw ImageLoadError{"Failed to read photometric interpretation."};
    }

    const uint16_t dataPhotometric = photometric;
    const uint16_t dataBitsPerSample = bitsPerSample;
    const uint16_t dataSampleFormat = sampleFormat;

    // Auto-convert LogLUV and LogL to RGB float. See http://www.anyhere.com/gward/pixformat/tiffluv.html
    if (photometric == PHOTOMETRIC_LOGLUV || photometric == PHOTOMETRIC_LOGL) {
        tlog::debug() << "Converting LogLUV/LogL to XYZ float.";

        if (compression != COMPRESSION_SGILOG && compression != COMPRESSION_SGILOG24) {
            throw ImageLoadError{"Unsupported compression for log data."};
        }

        if (!TIFFSetField(tif, TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT)) {
            throw ImageLoadError{"Failed to set SGI log data format."};
        }

        bitsPerSample = 32;
        sampleFormat = SAMPLEFORMAT_IEEEFP;
    }

    if (compression == COMPRESSION_PIXARLOG) {
        tlog::debug() << "Converting PIXAR log data to RGB float.";

        if (!TIFFSetField(tif, TIFFTAG_PIXARLOGDATAFMT, PIXARLOGDATAFMT_FLOAT)) {
            throw ImageLoadError{"Failed to set PIXAR log data format."};
        }

        bitsPerSample = 32;
        sampleFormat = SAMPLEFORMAT_IEEEFP;
    }

    // We will manually decompress JXL and JPEG2000 tiles further down the pipeline by invoking tev's JXL decoder directly on the
    // compressed data from the TIFF file. This returns fp32 data.
    static const uint16_t COMPRESSION_LOSSY_JPEG = 34892;
    const bool decodeRaw = compression == COMPRESSION_JXL_DNG_1_7 || compression == COMPRESSION_JXL || compression == COMPRESSION_JP2000 ||
        compression == COMPRESSION_JPEG || compression == COMPRESSION_LOSSY_JPEG;
    if (decodeRaw) {
        bitsPerSample = 32;
        sampleFormat = SAMPLEFORMAT_IEEEFP;
    }

    span<const uint8_t> jpegTables = tiffGetSpan<uint8_t>(tif, TIFFTAG_JPEGTABLES);
    if ((compression == COMPRESSION_JPEG || compression == COMPRESSION_LOSSY_JPEG) && !jpegTables.empty()) {
        tlog::debug() << "Found JPEG tables; will use for decompression.";
    }

    // DNG-specific photometric interpretations. See https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_0_0.pdf
    static const uint16_t PHOTOMETRIC_LINEAR_RAW = 34892;
    static const uint16_t PHOTOMETRIC_DEPTH = 51177;
    static const uint16_t PHOTOMETRIC_SEMANTIC = 52527;

    static const uint16_t SUPPORTED_PHOTOMETRICS[] = {
        PHOTOMETRIC_MINISBLACK,
        PHOTOMETRIC_MINISWHITE,
        PHOTOMETRIC_RGB,
        PHOTOMETRIC_PALETTE,
        PHOTOMETRIC_MASK,
        PHOTOMETRIC_YCBCR,
        //
        PHOTOMETRIC_CIELAB,
        PHOTOMETRIC_ICCLAB,
        PHOTOMETRIC_ITULAB,
        //
        PHOTOMETRIC_LOGLUV,
        PHOTOMETRIC_LOGL,
        PHOTOMETRIC_CFA, // Color Filter Array; displayed as grayscale for now
        // DNG-specific
        PHOTOMETRIC_LINEAR_RAW,
        PHOTOMETRIC_DEPTH,
        PHOTOMETRIC_SEMANTIC,
    };

    if (photometric == PHOTOMETRIC_SEPARATED) {
        throw ImageLoadError{"Separated images (e.g. CMYK) are unsupported."};
    }

    if (photometric == PHOTOMETRIC_YCBCR) {
        if (compression == COMPRESSION_JPEG || compression == COMPRESSION_LOSSY_JPEG || compression == COMPRESSION_JP2000) {
            // Our JPEG decoder upsamples YCbCr data for us
            TIFFUnsetField(tif, TIFFTAG_YCBCRSUBSAMPLING);

            if (compression == COMPRESSION_JP2000) {
                // Our JPEG2000 encoder furthermore outputs RGB directly
                photometric = PHOTOMETRIC_RGB;
                TIFFUnsetField(tif, TIFFTAG_REFERENCEBLACKWHITE);
            }
        }

        if (uint16_t subsampling[2]; TIFFGetField(tif, TIFFTAG_YCBCRSUBSAMPLING, &subsampling[0], &subsampling[1])) {
            tlog::debug() << fmt::format("Found YCbCr subsampling: {}x{}", subsampling[0], subsampling[1]);

            const bool hasSubsampling = subsampling[0] != 1 || subsampling[1] != 1;
            if (hasSubsampling) {
                // TODO: actually handle subsampling
                throw ImageLoadError{"Subsampled YCbCr images are only supported for JPEG-compressed TIFFs."};
            }
        }
    }

    if (all_of(begin(SUPPORTED_PHOTOMETRICS), end(SUPPORTED_PHOTOMETRICS), [&](uint16_t p) { return p != photometric; })) {
        throw ImageLoadError{fmt::format("Unsupported photometric interpretation: {}", photometric)};
    }

    uint16_t planar;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar)) {
        throw ImageLoadError{"Failed to read planar configuration."};
    }

    Vector2i size{(int)width, (int)height};
    if (size.x() == 0 || size.y() == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    static const uint16_t TIFFTAG_COLINTERLEAVEFACTOR = 52547;

    Vector2i interleave = {1, 1};
    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_COLINTERLEAVEFACTOR, TIFF_ANY)) {
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT: interleave.x() = tiffGetValue<uint16_t>(tif, TIFFTAG_COLINTERLEAVEFACTOR).value_or(1); break;
            case TIFF_LONG: interleave.x() = tiffGetValue<uint32_t>(tif, TIFFTAG_COLINTERLEAVEFACTOR).value_or(1); break;
            default: throw ImageLoadError{"Unsupported col interleave factor type."};
        }
    }

    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_ROWINTERLEAVEFACTOR, TIFF_ANY)) {
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT: interleave.y() = tiffGetValue<uint16_t>(tif, TIFFTAG_ROWINTERLEAVEFACTOR).value_or(1); break;
            case TIFF_LONG: interleave.y() = tiffGetValue<uint32_t>(tif, TIFFTAG_ROWINTERLEAVEFACTOR).value_or(1); break;
            default: throw ImageLoadError{"Unsupported row interleave factor type."};
        }
    }

    tlog::debug() << fmt::format(
        "TIFF info: size={} bps={}/{}/{} spp={} photometric={} planar={} interleave={} sampleFormat={} compression={}",
        size,
        tiffInternalBitsPerSample,
        dataBitsPerSample,
        bitsPerSample,
        samplesPerPixel,
        photometric,
        planar,
        interleave,
        sampleFormat,
        compression
    );

    // Check if we have an alpha channel
    bool hasAlpha = false;
    bool hasPremultipliedAlpha = true; // No alpha is treated as premultiplied
    size_t numExtraChannels = 0;

    if (const auto extraChannelTypes = tiffGetSpan<uint16_t>(tif, TIFFTAG_EXTRASAMPLES); !extraChannelTypes.empty()) {
        tlog::debug() << fmt::format("Found {} extra channels.", numExtraChannels);
        for (size_t i = 0; i < extraChannelTypes.size(); ++i) {
            if (extraChannelTypes[i] == EXTRASAMPLE_ASSOCALPHA || extraChannelTypes[i] == EXTRASAMPLE_UNASSALPHA) {
                if (hasAlpha) {
                    throw ImageLoadError{"Multiple alpha channels found."};
                }

                if (i != 0) {
                    throw ImageLoadError{"Alpha channel must be the first extra channel."};
                }

                hasAlpha = true;
                hasPremultipliedAlpha = extraChannelTypes[i] == EXTRASAMPLE_ASSOCALPHA;
            }
        }

        numExtraChannels = extraChannelTypes.size();
    } else if (samplesPerPixel == 2 || samplesPerPixel == 4) {
        tlog::warning() << "Assuming alpha channel for 2 or 4 samples per pixel.";
        numExtraChannels = 1;
        hasAlpha = true;
        hasPremultipliedAlpha = false; // Assume unassociated alpha if not specified
    } else {
        numExtraChannels = 0;
    }

    if (numExtraChannels >= samplesPerPixel) {
        throw ImageLoadError{fmt::format("Invalid number of extra channels: {}", numExtraChannels)};
    }

    // Determine number of color channels
    size_t numColorChannels = samplesPerPixel - numExtraChannels;
    const size_t numChannels = samplesPerPixel;

    size_t numRgbaChannels = numColorChannels + (hasAlpha ? 1 : 0);
    if (numRgbaChannels < 1 || numRgbaChannels > 4) {
        throw ImageLoadError{fmt::format("Unsupported number of RGBA channels: {}", numRgbaChannels)};
    }

    const size_t numNonRgbaChannels = numChannels - numRgbaChannels;

    const auto palette = tiffGetColorMap(tif);
    if (photometric == PHOTOMETRIC_PALETTE) {
        if (numColorChannels != 1) {
            throw ImageLoadError{"Palette images must have 1 color channel per pixel."};
        }

        // We'll read the palette and convert the single index channel to RGB later on, hence we need to keep track of the extra 2 channels
        numColorChannels += 2;
        numRgbaChannels += 2;

        if (sampleFormat != SAMPLEFORMAT_UINT) {
            throw ImageLoadError{"Palette images must have unsigned integer sample format."};
        }

        if (any_of(begin(palette), end(palette), [](const auto& c) { return c.empty(); })) {
            throw ImageLoadError{"Failed to read color palette."};
        }

        tlog::debug() << fmt::format("Read color palette with {} entries.", palette[0].size());
    }

    tlog::debug() << fmt::format("numRgbaChannels={} numNonRgbaChannels={}", numRgbaChannels, numNonRgbaChannels);

    const auto formatToPixelType = [](uint16_t sampleFormat) {
        switch (sampleFormat) {
            case SAMPLEFORMAT_UINT: return EPixelType::Uint;
            case SAMPLEFORMAT_INT: return EPixelType::Int;
            case SAMPLEFORMAT_IEEEFP: return EPixelType::Float;
            default: throw ImageLoadError{fmt::format("Unsupported sample format: {}", sampleFormat)};
        }
    };

    const auto deriveScale = [](EPixelType pixelType, size_t bitsPerSample) {
        switch (pixelType) {
            case EPixelType::Uint: return 1.0f / (float)((1ull << bitsPerSample) - 1);
            case EPixelType::Int: return 1.0f / (float)((1ull << (bitsPerSample - 1)) - 1);
            case EPixelType::Float: return 1.0f;
            default: throw ImageLoadError{fmt::format("Unsupported pixel type: {}", toString(pixelType))};
        }
    };

    ImageData resultData;
    resultData.partName = partName;
    resultData.dataWindow = resultData.displayWindow = size;

    uint16_t orientation = 1;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation)) {
        throw ImageLoadError{"Failed to read orientation."};
    }

    resultData.orientation = static_cast<EOrientation>(orientation);
    resultData.hasPremultipliedAlpha = hasPremultipliedAlpha;

    // Read ICC profile if available
    uint32_t iccProfileSize = 0;
    void* iccProfileData = nullptr;

    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccProfileSize, &iccProfileData) && iccProfileSize > 0 && iccProfileData) {
        tlog::debug() << fmt::format("Found ICC color profile of size {} bytes", iccProfileSize);
    }

    // Read XMP metadata if available
    uint32_t xmpDataSize = 0;
    const char* xmpData = nullptr;

    if (TIFFGetField(tif, TIFFTAG_XMLPACKET, &xmpDataSize, &xmpData) && xmpDataSize > 0 && xmpData) {
        tlog::debug() << fmt::format("Found XMP metadata of size {} bytes", xmpDataSize);
        try {
            const Xmp xmp{
                string_view{xmpData, xmpDataSize}
            };

            resultData.attributes.emplace_back(xmp.attributes());
        } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to parse XMP data: {}", e.what()); }
    }

    // TIFF images are either broken into strips (original format) or tiles (starting with TIFF 6.0). In practice, strips are just tiles
    // with the same width as the image, allowing us to share quite a bit of code below.
    const bool isTiled = TIFFIsTiled(tif);

    struct TileInfo {
        size_t rawSize, size, rowSize, count, numX, numY;
        uint32_t width, height;
    } tile;
    const size_t numPlanes = planar == PLANARCONFIG_CONTIG ? 1 : samplesPerPixel;
    if (isTiled) {
        const uint64_t* rawTileSize = NULL;
        TIFFGetField(tif, TIFFTAG_TILEBYTECOUNTS, &rawTileSize);
        tile.rawSize = rawTileSize ? *rawTileSize : 0;

        tile.size = TIFFTileSize64(tif);
        tile.rowSize = TIFFTileRowSize64(tif);
        tile.count = TIFFNumberOfTiles(tif);

        if (!TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile.width) || !TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile.height)) {
            throw ImageLoadError{"Failed to read tile dimensions."};
        }

        uint32_t tileDepth = 0;
        if (TIFFGetField(tif, TIFFTAG_TILEDEPTH, &tileDepth) && tileDepth != 1) {
            throw ImageLoadError{"3D tiled images are not supported."};
        }

        tile.numX = (size.x() + tile.width - 1) / tile.width;
        tile.numY = (size.y() + tile.height - 1) / tile.height;
    } else {
        tile.size = TIFFStripSize64(tif);
        tile.rowSize = TIFFScanlineSize64(tif);
        tile.count = TIFFNumberOfStrips(tif);

        // Strips are just tiles with the same width as the image and variable height.
        tile.width = size.x();
        tile.height = tile.size / tile.rowSize;

        tile.numX = 1;
        tile.numY = (size.y() + tile.height - 1) / tile.height;
    }

    const auto readTile = isTiled ? TIFFReadEncodedTile : TIFFReadEncodedStrip;

    const auto getRawTileSpan = [&tiffData, &tile, isTiled](TIFF* tif, uint32_t tileIndex) -> span<const uint8_t> {
        if (tileIndex >= tile.count) {
            throw ImageLoadError{fmt::format("Tile index {} out of bounds ({} tiles total)", tileIndex, tile.count)};
        }

        const auto getRawTileOffset = [isTiled](TIFF* tif, uint32_t tileIndex) -> uint64_t {
            const uint64_t* rawTileOffset = NULL;
            if (!TIFFGetField(tif, isTiled ? TIFFTAG_TILEOFFSETS : TIFFTAG_STRIPOFFSETS, &rawTileOffset) || !rawTileOffset) {
                throw ImageLoadError{fmt::format("Failed to read raw tile offset for tile {}", tileIndex)};
            }

            return rawTileOffset[tileIndex];
        };

        const auto getRawTileSize = [isTiled](TIFF* tif, uint32_t tileIndex) -> size_t {
            const uint64_t* rawTileSize = NULL;
            if (!TIFFGetField(tif, isTiled ? TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS, &rawTileSize) || !rawTileSize) {
                throw ImageLoadError{fmt::format("Failed to read raw tile size for tile {}", tileIndex)};
            }

            return (size_t)rawTileSize[tileIndex];
        };

        const size_t offset = getRawTileOffset(tif, tileIndex);
        if (offset == 0) {
            throw ImageLoadError{fmt::format("Raw tile offset is 0 for tile {}", tileIndex)};
        }

        const size_t size = getRawTileSize(tif, tileIndex);
        if (size == 0) {
            throw ImageLoadError{fmt::format("Raw tile size is 0 for tile {}", tileIndex)};
        }

        if ((size_t)tiffData.size < offset + size) {
            throw ImageLoadError{fmt::format(
                "Raw tile data for tile {} is out of bounds: offset={} size={} dataSize={}", tileIndex, offset, size, tiffData.size
            )};
        }

        return span<const uint8_t>{(const uint8_t*)tiffData.data + offset, size};
    };

    // Be robust against broken TIFFs that have a tile/strip size smaller than the actual data size. Make sure to allocate enough memory
    // to fit all data.
    tile.size = std::max(tile.size, (size_t)tile.width * tile.height * bitsPerSample * samplesPerPixel / numPlanes / 8);

    tlog::debug() << fmt::format(
        "tile: size={} count={} width={} height={} numX={} numY={}", tile.size, tile.count, tile.width, tile.height, tile.numX, tile.numY
    );

    if (planar == PLANARCONFIG_SEPARATE && tile.count % samplesPerPixel != 0) {
        throw ImageLoadError{"Number of tiles/strips is not a multiple of samples per pixel."};
    }

    if (tile.count != tile.numX * tile.numY * numPlanes) {
        throw ImageLoadError{fmt::format(
            "Number of tiles/strips does not match expected dimensions. Expected {}x{}x{}={} tiles, got {}.",
            tile.numX,
            tile.numY,
            numPlanes,
            tile.numX * tile.numY * numPlanes,
            tile.count
        )};
    }

    HeapArray<uint8_t> tileData(tile.size * tile.count);

    const size_t numTilesPerPlane = tile.count / numPlanes;
    // We'll unpack the bits into 32-bit or 64-bit unsigned integers first, then convert to float. This simplifies the bit unpacking
    const int unpackedBitsPerSample = bitsPerSample > 32 ? 64 : 32;

    const size_t unpackedTileRowSamples = tile.width * samplesPerPixel / numPlanes;
    const size_t unpackedTileSize = tile.height * unpackedTileRowSamples * unpackedBitsPerSample / 8;
    HeapArray<uint8_t> unpackedTile(unpackedTileSize * tile.count);

    const bool handleSign = sampleFormat == SAMPLEFORMAT_INT;

    vector<Task<void>> decodeTasks;

    // Read tiled/striped data. Unfortunately, libtiff doesn't support reading all tiles/strips in parallel, so we have to do that
    // sequentially.
    HeapArray<uint8_t> imageData((size_t)size.x() * size.y() * samplesPerPixel * unpackedBitsPerSample / 8);

    for (size_t i = 0; i < tile.count; ++i) {
        uint8_t* const td = tileData.data() + tile.size * i;

        if (decodeRaw) {
            const auto compressedTileData = getRawTileSpan(tif, (uint32_t)i);
            decodeTasks.emplace_back(
                ThreadPool::global().enqueueCoroutine(
                    [&, i, compressedTileData = std::move(compressedTileData)]() -> Task<void> {
                        // Assume the embedded data has the same bits/format as the TIFF wrapper claims (can be overridden by the loader)
                        size_t nestedBitsPerSample = dataBitsPerSample;
                        EPixelType nestedPixelType = formatToPixelType(dataSampleFormat);

                        vector<ImageData> tmp;
                        switch (compression) {
                            case COMPRESSION_JXL_DNG_1_7:
                            case COMPRESSION_JXL: {
                                const auto loader = JxlImageLoader{};
                                tmp = co_await loader.load(
                                    compressedTileData, "", "", {}, priority, false, &nestedBitsPerSample, &nestedPixelType
                                );
                            } break;
                            case COMPRESSION_JP2000: {
                                const auto loader = Jpeg2000ImageLoader{};
                                tmp = co_await loader.load(
                                    compressedTileData, "", "", {}, priority, false, &nestedBitsPerSample, &nestedPixelType
                                );
                            } break;
                            case COMPRESSION_JPEG:
                            case COMPRESSION_LOSSY_JPEG: {
                                nestedPixelType = EPixelType::Uint;
                                tmp.emplace_back(
                                    co_await decodeJpeg(
                                        compressedTileData,
                                        jpegTables,
                                        {(int)tile.width, (int)tile.height},
                                        samplesPerPixel / numPlanes,
                                        &nestedBitsPerSample,
                                        dataPhotometric,
                                        priority
                                    )
                                );
                            } break;
                            default: throw ImageLoadError{fmt::format("Unsupported compression type: {}", compression)};
                        }

                        if (tmp.size() != 1) {
                            throw ImageLoadError{fmt::format("Expected exactly one image from tile, got {}", tmp.size())};
                        }

                        auto& tmpImage = tmp.front();

                        if (tmpImage.channels.size() < (size_t)samplesPerPixel / numPlanes) {
                            throw ImageLoadError{
                                fmt::format("Tile has too few channels: expected {}, got {}", numPlanes, tmpImage.channels.size())
                            };
                        }

                        const auto tileSize = Vector2i{(int)tile.width, (int)tile.height};
                        for (auto& channel : tmpImage.channels) {
                            if (channel.size() != tileSize) {
                                throw ImageLoadError{fmt::format(
                                    "Tile channel '{}' has unexpected dimensions: expected {}, got {}",
                                    channel.name(),
                                    tileSize,
                                    tmpImage.channels.front().size()
                                )};
                            }
                        }

                        // Rescale embedded image data according to its true bits per sample. E.g. when a 10 bit TIFF encodes data with only
                        // 16 bits of JXL precision, as can happen when JXL codestreams are embedded in TIFF files. Curiously, when the
                        // situation is reversed (e.g. 16 bit TIFF with 14 bit JXL data), the data shouldn't get rescaled. Hence the clamp.
                        const float scale = std::max(
                            1.0f,
                            deriveScale(formatToPixelType(dataSampleFormat), dataBitsPerSample) /
                                deriveScale(nestedPixelType, nestedBitsPerSample)
                        );

                        const size_t planeTile = i % numTilesPerPlane;
                        const size_t tileX = planeTile % tile.numX;
                        const size_t tileY = planeTile / tile.numX;

                        const int xStart = (int)tileX * tile.width;
                        const int xEnd = std::min((int)((tileX + 1) * tile.width), size.x());

                        const int yStart = (int)tileY * tile.height;
                        const int yEnd = std::min((int)((tileY + 1) * tile.height), size.y());

                        const size_t numPixels = (size_t)(xEnd - xStart) * (yEnd - yStart);

                        auto* const data = (float*)imageData.data();

                        const auto views = tmpImage.channels | views::transform([](Channel& c) { return c.view<float>(); }) | to_vector;

                        co_await ThreadPool::global().parallelForAsync<int>(
                            yStart,
                            yEnd,
                            numPixels * samplesPerPixel / numPlanes,
                            [&](int y) {
                                const int y0 = y - yStart;
                                if (planar == PLANARCONFIG_CONTIG) {
                                    for (int x = xStart; x < xEnd; ++x) {
                                        const int x0 = x - xStart;
                                        for (int c = 0; c < samplesPerPixel; ++c) {
                                            const auto pixel = views[c][x0, y0];
                                            data[(y * size.x() + x) * samplesPerPixel + c] = pixel * scale;
                                        }
                                    }
                                } else {
                                    size_t c = i / numTilesPerPlane;
                                    for (int x = xStart; x < xEnd; ++x) {
                                        const int x0 = x - xStart;
                                        const auto pixel = views[0][x0, y0];
                                        data[(y * size.x() + x) * samplesPerPixel + c] = pixel * scale;
                                    }
                                }
                            },
                            priority
                        );
                    },
                    priority
                )
            );

            continue;
        }

        if (readTile(tif, (uint32_t)i, td, tile.size) < 0) {
            co_await awaitAll(decodeTasks);
            throw ImageLoadError{fmt::format("Failed to read tile {}", i)};
        }

        decodeTasks.emplace_back(
            ThreadPool::global().enqueueCoroutine(
                [&, i, td]() -> Task<void> {
                    uint8_t* const utd = unpackedTile.data() + unpackedTileSize * i;

                    const size_t planeTile = i % numTilesPerPlane;
                    const size_t tileX = planeTile % tile.numX;
                    const size_t tileY = planeTile / tile.numX;

                    const int xStart = (int)tileX * tile.width;
                    const int xEnd = std::min((int)((tileX + 1) * tile.width), size.x());

                    const int yStart = (int)tileY * tile.height;
                    const int yEnd = std::min((int)((tileY + 1) * tile.height), size.y());

                    const size_t numPixels = (size_t)tile.width * tile.height;

                    const auto unpackTask = [&](auto* const utd, auto* const data) -> Task<void> {
                        co_await ThreadPool::global().parallelForAsync<int>(
                            yStart,
                            yEnd,
                            numPixels * samplesPerPixel / numPlanes,
                            [&](int y) {
                                int y0 = y - yStart;
                                unpackBits(
                                    td + tile.rowSize * y0,
                                    tile.rowSize,
                                    bitsPerSample,
                                    utd + unpackedTileRowSamples * y0,
                                    unpackedTileRowSamples,
                                    handleSign
                                );

                                if (planar == PLANARCONFIG_CONTIG) {
                                    for (int x = xStart; x < xEnd; ++x) {
                                        for (int c = 0; c < samplesPerPixel; ++c) {
                                            auto pixel = utd[(y0 * tile.width + x - xStart) * samplesPerPixel + c];
                                            data[(y * size.x() + x) * samplesPerPixel + c] = pixel;
                                        }
                                    }
                                } else {
                                    size_t c = i / numTilesPerPlane;
                                    for (int x = xStart; x < xEnd; ++x) {
                                        auto pixel = utd[y0 * tile.width + x - xStart];
                                        data[(y * size.x() + x) * samplesPerPixel + c] = pixel;
                                    }
                                }
                            },
                            priority
                        );
                    };

                    if (unpackedBitsPerSample > 32) {
                        co_await unpackTask((uint64_t*)utd, (uint64_t*)imageData.data());
                    } else {
                        co_await unpackTask((uint32_t*)utd, (uint32_t*)imageData.data());
                    }
                },
                priority
            )
        );
    }

    co_await awaitAll(decodeTasks);

    if (interleave != Vector2i{1, 1}) {
        HeapArray<uint8_t> interleavedImageData;
        const size_t bytesPerSample = unpackedBitsPerSample / 8;
        const size_t numPixels = (size_t)size.x() * size.y();

        interleavedImageData = HeapArray<uint8_t>(numPixels * numChannels * bytesPerSample);

        const auto parallelInterleave = [&](const auto* in, auto* out) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                numPixels * numChannels,
                [&](int y) {
                    const size_t subY = y / interleave.y();
                    const size_t interleaveY = y - subY * interleave.y();
                    const size_t srcY = interleaveY * (size.y() / interleave.y()) + subY;

                    for (int x = 0; x < size.x(); ++x) {
                        const size_t subX = x / interleave.x();
                        const size_t interleaveX = x - subX * interleave.x();
                        const size_t srcX = interleaveX * (size.x() / interleave.x()) + subX;

                        const size_t srcIndexBase = (srcY * size.x() + srcX) * numChannels;
                        const size_t dstIndexBase = (y * size.x() + x) * numChannels;
                        for (size_t c = 0; c < numChannels; ++c) {
                            out[dstIndexBase + c] = in[srcIndexBase + c];
                        }
                    }
                },
                priority
            );
        };

        if (bytesPerSample == 8) {
            co_await parallelInterleave((const uint64_t*)imageData.data(), (uint64_t*)interleavedImageData.data());
        } else if (bytesPerSample == 4) {
            co_await parallelInterleave((const uint32_t*)imageData.data(), (uint32_t*)interleavedImageData.data());
        } else {
            throw runtime_error{"Unsupported bytes per sample."};
        }

        imageData = std::move(interleavedImageData);
    }

    if (const auto activeArea = getActiveArea(tif, size); size != activeArea.size()) {
        const auto rawSize = size;
        size = activeArea.size();
        const auto numPixels = (size_t)size.x() * size.y();

        tlog::debug() << fmt::format("Cropping to active area: [{},{}] -> {}", activeArea.min, activeArea.max, size);

        resultData.dataWindow = resultData.displayWindow = size;

        HeapArray<uint8_t> croppedImageData((size_t)numPixels * samplesPerPixel * unpackedBitsPerSample / 8);

        const auto cropToActiveArea = [&](auto* const croppedData, const auto* const data) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                numPixels * numChannels,
                [&](int y) {
                    const int srcY = y + activeArea.min.y();
                    for (int x = 0; x < size.x(); ++x) {
                        const int srcX = x + activeArea.min.x();
                        for (size_t c = 0; c < numChannels; ++c) {
                            croppedData[((size_t)y * size.x() + x) * numChannels + c] =
                                data[((size_t)srcY * rawSize.x() + srcX) * numChannels + c];
                        }
                    }
                },
                priority
            );
        };

        if (unpackedBitsPerSample == 64) {
            co_await cropToActiveArea((uint64_t*)croppedImageData.data(), (const uint64_t*)imageData.data());
        } else if (unpackedBitsPerSample == 32) {
            co_await cropToActiveArea((uint32_t*)croppedImageData.data(), (const uint32_t*)imageData.data());
        } else {
            throw ImageLoadError{fmt::format("Unsupported unpacked bits per sample: {}", unpackedBitsPerSample)};
        }

        imageData = std::move(croppedImageData);
    }

    resultData.displayWindow = getDefaultCrop(tif, size);

    size_t numInterleavedChannels = nextSupportedTextureChannelCount(numRgbaChannels);

    // Local scope to prevent use-after-move
    {
        const auto desiredPixelFormat = bitsPerSample > 16 ? EPixelFormat::F32 : EPixelFormat::F16;
        auto rgbaChannels = co_await ImageLoader::makeRgbaInterleavedChannels(
            numRgbaChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, desiredPixelFormat, partName, priority
        );
        auto extraChannels = ImageLoader::makeNChannels(numNonRgbaChannels, size, EPixelFormat::F32, desiredPixelFormat, partName);

        resultData.channels.insert(resultData.channels.end(), make_move_iterator(rgbaChannels.begin()), make_move_iterator(rgbaChannels.end()));
        resultData.channels.insert(
            resultData.channels.end(), make_move_iterator(extraChannels.begin()), make_move_iterator(extraChannels.end())
        );
    }

    const float intConversionScale = deriveScale(formatToPixelType(sampleFormat), dataBitsPerSample);

    ETiffKind kind = ETiffKind::U32;
    switch (sampleFormat) {
        case SAMPLEFORMAT_IEEEFP: {
            switch (bitsPerSample) {
                case 16: kind = ETiffKind::F16; break;
                case 24: kind = ETiffKind::F24; break;
                case 32: kind = ETiffKind::F32; break;
                case 64: kind = ETiffKind::F64; break;
                default: throw ImageLoadError{fmt::format("Unsupported fp bps={}", bitsPerSample)};
            }

            break;
        }
        case SAMPLEFORMAT_INT: {
            kind = ETiffKind::I32;
            break;
        }
        case SAMPLEFORMAT_UINT: {
            if (photometric == PHOTOMETRIC_PALETTE) {
                kind = ETiffKind::Palette;
            } else {
                kind = ETiffKind::U32;
            }

            break;
        }
        default: throw ImageLoadError{fmt::format("Unsupported sample format: {}", sampleFormat)};
    }

    if (kind == ETiffKind::F16 || kind == ETiffKind::F24) {
        tlog::debug() << "Converting 16/24 bit float data to 32 bit float.";
        co_await convertF16AndF24ToF32(kind, (uint32_t*)imageData.data(), numChannels, size, priority);
        kind = ETiffKind::F32;
    }

    const bool flipWhiteAndBlack = photometric == PHOTOMETRIC_MINISWHITE;

    // Convert all the extra channels to float and store them in the result data. No further processing needed.
    for (size_t c = numChannels - numExtraChannels + (hasAlpha ? 1 : 0); c < numChannels; ++c) {
        co_await tiffDataToFloat32<false>(
            kind,
            interleave,
            palette,
            (uint32_t*)(imageData.data() + c * unpackedBitsPerSample / 8),
            numChannels,
            resultData.channels[c].view<float>(),
            false,
            priority,
            intConversionScale,
            flipWhiteAndBlack
        );
    }

    const set<uint16_t> labPhotometrics = {
        PHOTOMETRIC_CIELAB,
        PHOTOMETRIC_ICCLAB,
        PHOTOMETRIC_ITULAB,
    };

    auto rgbaView = MultiChannelView<float>{span{resultData.channels}.subspan(0, numRgbaChannels)};

    // The RGBA channels might need color space conversion: store them in a staging buffer first and then try ICC conversion. ICC profiles
    // are generally most accurate when available, so prefer them. However, if we've got a Lab photometric interpretation, TIFF's data handling
    // can be tricky and we can reproduce the exact behavior the ICC would have without too much trouble ourselves, so skip ICC in that case.
    if (iccProfileData && iccProfileSize > 0 && !labPhotometrics.contains(photometric)) {
        try {
            co_await tiffDataToFloat32<false>(
                kind, interleave, palette, (uint32_t*)imageData.data(), numChannels, rgbaView, hasAlpha, priority, intConversionScale, flipWhiteAndBlack
            );

            const auto profile = ColorProfile::fromIcc({(uint8_t*)iccProfileData, iccProfileSize});
            co_await toLinearSrgbPremul(
                profile,
                hasAlpha ? (hasPremultipliedAlpha ? EAlphaKind::Premultiplied : EAlphaKind::Straight) : EAlphaKind::None,
                rgbaView,
                rgbaView,
                nullopt,
                priority
            );

            resultData.hasPremultipliedAlpha = true;
            resultData.readMetadataFromIcc(profile);

            co_return resultData;
        } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    co_await tiffDataToFloat32<false>(
        kind, interleave, palette, (uint32_t*)imageData.data(), numChannels, rgbaView, hasAlpha, priority, intConversionScale, flipWhiteAndBlack
    );

    // Both CFA and linear raw DNG data need to be linearized and normalized prior to color space conversions. Metadata for
    // linearization assumes *pre* demosaicing data, so this step needs to be done before we convert CFA to RGB.
    if ((isDng && photometric == PHOTOMETRIC_CFA) || photometric == PHOTOMETRIC_LINEAR_RAW) {
        co_await linearizeAndNormalizeRawDng(tif, dataSampleFormat, dataBitsPerSample, rgbaView, priority);
    }

    if (photometric == PHOTOMETRIC_CFA) {
        if (samplesPerPixel != 1 || numColorChannels != 1 || numRgbaChannels != 1) {
            throw ImageLoadError{"CFA images must have exactly 1 sample per pixel / color / rgba channel."};
        }

        numRgbaChannels = numColorChannels = samplesPerPixel = 3;
        numInterleavedChannels = nextSupportedTextureChannelCount(numRgbaChannels);
        auto rgbaChannels = co_await ImageLoader::makeRgbaInterleavedChannels(
            numRgbaChannels, numInterleavedChannels, false, size, EPixelFormat::F32, resultData.channels.front().desiredPixelFormat(), partName, priority
        );

        co_await demosaicCfa(tif, resultData.channels.front().view<float>(), MultiChannelView<float>(rgbaChannels), priority);

        photometric = isDng ? PHOTOMETRIC_LINEAR_RAW : PHOTOMETRIC_RGB;

        resultData.channels.front().setName(Channel::joinIfNonempty(partName, "cfa.L"));
        resultData.channels.insert(
            resultData.channels.begin(), make_move_iterator(rgbaChannels.begin()), make_move_iterator(rgbaChannels.end())
        );

        // Update the view to point to the new RGBA channels
        rgbaView = MultiChannelView<float>{span{resultData.channels}.subspan(0, numRgbaChannels)};
    }

    // If no ICC profile is available, we can try to convert the color space manually using TIFF's chromaticity data and transfer function.
    if (compression == COMPRESSION_PIXARLOG) {
        // If we're a Pixar log image, the data is already linear
    } else if (photometric == PHOTOMETRIC_LINEAR_RAW) {
        co_await postprocessLinearRawDng(tif, rgbaView, resultData, reverseEndian, settings.dngApplyCameraProfile, priority);
    } else if (photometric == PHOTOMETRIC_LOGLUV || photometric == PHOTOMETRIC_LOGL) {
        // If we're a LogLUV image, we've already configured the encoder to give us linear XYZ data, so we can just convert that to Rec.709.
        resultData.toRec709 = xyzToChromaMatrix(rec709Chroma());
    } else if (photometric <= PHOTOMETRIC_PALETTE || photometric == PHOTOMETRIC_YCBCR) {
        co_await postprocessRgb(tif, photometric, dataBitsPerSample, numColorChannels, rgbaView, resultData, priority);
    } else if (labPhotometrics.contains(photometric)) {
        co_await postprocessLab(tif, photometric, dataBitsPerSample, numColorChannels, rgbaView, resultData, priority);
    } else {
        // Other photometric interpretations do not need a transfer
        resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;
    }

    co_return resultData;
}

Task<vector<ImageData>>
    TiffImageLoader::load(istream& iStream, const fs::path& path, string_view, const ImageLoaderSettings& settings, int priority) const {
    // This function tries to implement the most relevant parts of the TIFF 6.0 spec:
    // https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf
    char magic[4] = {0};
    iStream.read(magic, sizeof(magic));
    if (!iStream || (magic[0] != 'I' && magic[0] != 'M') || magic[1] != magic[0]) {
        throw FormatNotSupported{"File is not a TIFF image."};
    }

    const auto fileEndianness = magic[0] == 'I' ? endian::little : endian::big;
    const bool reverseEndian = fileEndianness != endian::native;

    const uint16_t answer = reverseEndian ? (magic[2] << 8 | magic[3]) : (magic[3] << 8 | magic[2]);
    if (answer != 42) {
        throw FormatNotSupported{"File is not a TIFF image."};
    }

    TIFFSetErrorHandler(tiffErrorHandler);
    TIFFSetWarningHandler(tiffWarningHandler);

    // Read the entire stream into memory and decompress from there. Technically, we can progressively decode TIFF images, but we want
    // to additionally load the TIFF image via our EXIF library, which requires the file to be in memory. For the same reason, we also
    // prepend the EXIF FOURCC to the data ahead of the TIFF header.
    iStream.seekg(0, ios::end);
    const size_t fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    HeapArray<uint8_t> buffer(fileSize + sizeof(Exif::FOURCC));
    copy(Exif::FOURCC.begin(), Exif::FOURCC.end(), buffer.data());
    iStream.read((char*)buffer.data() + sizeof(Exif::FOURCC), fileSize);

    optional<AttributeNode> exifAttributes;
    try {
        const auto exif = Exif{buffer};
        exifAttributes = exif.toAttributes();
    } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }

    TiffData tiffData(buffer.data() + sizeof(Exif::FOURCC), fileSize);
    TIFF* tif = TIFFClientOpen(
        toString(path).c_str(),
        "rMc", // read-only w/ memory mapping; no strip chopping
        static_cast<thandle_t>(&tiffData),
        tiffReadProc,
        tiffWriteProc,
        tiffSeekProc,
        tiffCloseProc,
        tiffSizeProc,
        tiffMapProc,
        tiffUnmapProc
    );

    if (!tif) {
        throw ImageLoadError{"Failed to open TIFF image."};
    }

    ScopeGuard tiffGuard{[tif] { TIFFClose(tif); }};

    bool isDng = false;
    uint8_t* dngVersion = nullptr;
    if (TIFFGetField(tif, TIFFTAG_DNGVERSION, &dngVersion)) {
        tlog::debug() << fmt::format("Detected DNG {}.{}.{}.{} file", dngVersion[0], dngVersion[1], dngVersion[2], dngVersion[3]);
        isDng = true;
    }

    enum EDngSubfileType : uint32_t {
        Main = 0,
        Reduced = 1,
        Transparency = 4,
        TransparencyReduced = 5,
        Depth = 8,
        DepthReduced = 9,
        Enhanced = 16,
        AltReduced = 65537,
        SemanticMask = 65540,
    };

    const auto dngSubFileTypeToString = [](uint32_t subFileType) -> string {
        switch (subFileType) {
            case Main: return "";
            case Reduced: return "reduced";
            case Transparency: return "transparency";
            case TransparencyReduced: return "reduced.transparency";
            case Depth: return "depth";
            case DepthReduced: return "reduced.depth";
            case Enhanced: return "enhanced";
            case AltReduced: return "reduced.alt";
            case SemanticMask: return "mask";
            default: return fmt::format("unknown.{}", subFileType);
        }
    };

    const auto isThumbnail = [](uint32_t subFileType) { return (subFileType & 1) != 0; };

    // The following code reads all images contained in main-IDFs and sub-IDFs of the TIFF file as per https://libtiff.gitlab.io/libtiff/multi_page.html
    vector<ImageData> result;

    EOrientation dngOrientation = EOrientation::None;
    const auto tryLoadImage = [&](tdir_t dir, int subId, int subChainId) -> Task<void> {
        string name = subId != -1 ? fmt::format("ifd.{}.subifd.{}.{}", dir, subId, subChainId) : fmt::format("ifd.{}", dir);
        if (isDng) {
            if (dngOrientation == EOrientation::None) {
                uint16_t orientation = 0;
                if (TIFFGetField(tif, TIFFTAG_ORIENTATION, &orientation) && orientation <= 8) {
                    dngOrientation = (EOrientation)orientation;
                }
            }

            if (EDngSubfileType type; TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &type)) {
                // DNGs often come with multiple thumbnail images that act as a loading preview in photo editing software. Uninteresting for
                // tev to load, except for the main IFD's orientation tag, which is authorative.
                if (isThumbnail(type)) {
                    // co_return;
                }

                name = dngSubFileTypeToString(type);
            }
        }

        try {
            tlog::debug() << fmt::format("Loading '{}'", name);

            ImageData& data = result.emplace_back(co_await readTiffImage(tiffData, tif, isDng, reverseEndian, name, settings, priority));
            if (exifAttributes) {
                data.attributes.emplace_back(exifAttributes.value());
            }

            // Propagate orientation from the main image to sub-images if loading a DNG
            if (dngOrientation != EOrientation::None) {
                data.orientation = dngOrientation;
            }
        } catch (const ImageLoadError& e) { tlog::warning() << fmt::format("Failed to load '{}': {}", name, e.what()); }
    };

    // The first directory is already read through TIFFOpen()
    do {
        const auto currentDirOffset = TIFFCurrentDirOffset(tif);
        const auto currentDirNumber = TIFFCurrentDirectory(tif);

        co_await tryLoadImage(currentDirNumber, -1, -1);

        // Check if the current top-level IFD has sub-IFDs. If so, visit them before moving on to next top-level IDF.
        if (const auto offsets = tiffGetSpan<toff_t>(tif, TIFFTAG_SUBIFD); !offsets.empty()) {
            const vector<toff_t> subIfdOffsets(offsets.begin(), offsets.end()); // copy to avoid invalidating the span
            for (size_t i = 0; i < subIfdOffsets.size(); i++) {
                if (!TIFFSetSubDirectory(tif, subIfdOffsets[i])) {
                    throw ImageLoadError{"Failed to read sub IFD."};
                }

                int j = 0;
                do {
                    co_await tryLoadImage(currentDirNumber, i, j);
                    ++j;
                } while (TIFFReadDirectory(tif));
            }

            // Go back to main-IFD chain and re-read that main-IFD directory
            if (!TIFFSetSubDirectory(tif, currentDirOffset)) {
                throw ImageLoadError{"Failed to read main IFD."};
            }
        }
    } while (TIFFReadDirectory(tif)); // Read next main-IFD directory (subfile)

    if (result.empty()) {
        throw ImageLoadError{"No images found in TIFF file."};
    }

    // Ensure earlier IFDs appear before later ones, as well as main images before reduced images in DNGs
    sort(begin(result), end(result), [](const ImageData& a, const ImageData& b) { return a.partName < b.partName; });

    // If we're a DNG, auxiliary images are either extra channels (depth, transparency, semantic mask) or reduced-resolution or enhanced
    // versions of the main image. They are often smaller-resolution as the main image, but they should nonetheless be treated as extra
    // channels rather than separate images. Hence: match colors, resize, and flatten into single image.
    if (isDng) {
        auto& mainImage = result.front();

        vector<Task<void>> moveTasks;
        for (size_t i = 1; i < result.size(); ++i) {
            moveTasks.emplace_back(result[i].matchColorsAndSizeOf(mainImage, priority));
        }

        co_await awaitAll(moveTasks);

        for (size_t i = 1; i < result.size(); ++i) {
            mainImage.channels.insert(
                mainImage.channels.end(), make_move_iterator(result[i].channels.begin()), make_move_iterator(result[i].channels.end())
            );
        }

        result.resize(1);
    }

    co_return result;
}

} // namespace tev
