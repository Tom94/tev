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

#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Demosaic.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/Jpeg2000ImageLoader.h>
#include <tev/imageio/JxlImageLoader.h>
#include <tev/imageio/TiffImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <half.h>
#include <tiff.h>
#include <tiffio.h>

#include <span>

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

template <bool SRGB_TO_LINEAR = false>
Task<void> tiffDataToFloat32(
    ETiffKind kind,
    const Vector2i& interleave,
    const uint16_t* __restrict palette[3],
    uint32_t* __restrict imageData,
    size_t numSppIn,
    float* __restrict floatData,
    size_t numSppOut,
    const nanogui::Vector2i& size,
    bool hasAlpha,
    int priority,
    float scale,
    bool flipWhiteAndBlack
) {
    HeapArray<uint8_t> interleavedImageData;
    if (interleave != Vector2i{1, 1}) {
        const size_t bytesPerSample = kind == ETiffKind::F64 ? 8 : 4;
        const size_t numPixels = (size_t)size.x() * size.y();

        interleavedImageData = HeapArray<uint8_t>(numPixels * numSppIn * bytesPerSample);

        const auto parallelInterleave = [&](const auto* in, auto* out) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                numPixels * numSppIn,
                [&](int y) {
                    const size_t subY = y / interleave.y();
                    const size_t interleaveY = y - subY * interleave.y();
                    const size_t srcY = interleaveY * (size.y() / interleave.y()) + subY;

                    for (int x = 0; x < size.x(); ++x) {
                        const size_t subX = x / interleave.x();
                        const size_t interleaveX = x - subX * interleave.x();
                        const size_t srcX = interleaveX * (size.x() / interleave.x()) + subX;

                        const size_t srcIndexBase = (srcY * size.x() + srcX) * numSppIn;
                        const size_t dstIndexBase = (y * size.x() + x) * numSppIn;
                        for (size_t c = 0; c < numSppIn; ++c) {
                            out[dstIndexBase + c] = in[srcIndexBase + c];
                        }
                    }
                },
                priority
            );
        };

        if (bytesPerSample == 8) {
            co_await parallelInterleave((const uint64_t*)imageData, (uint64_t*)interleavedImageData.data());
        } else if (bytesPerSample == 4) {
            co_await parallelInterleave((const uint32_t*)imageData, (uint32_t*)interleavedImageData.data());
        } else {
            throw runtime_error{"Unsupported bytes per sample."};
        }

        imageData = (uint32_t*)interleavedImageData.data();
    }

    // Convert lower-bit depth float formats to 32 bit
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
    }

    if (kind == ETiffKind::F64) {
        co_await toFloat32<double, SRGB_TO_LINEAR>((const double*)imageData, numSppIn, floatData, numSppOut, size, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::F32) {
        co_await toFloat32<float, SRGB_TO_LINEAR>((const float*)imageData, numSppIn, floatData, numSppOut, size, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::I32) {
        co_await toFloat32<int32_t, SRGB_TO_LINEAR>((const int32_t*)imageData, numSppIn, floatData, numSppOut, size, hasAlpha, priority, scale);
    } else if (kind == ETiffKind::U32) {
        co_await toFloat32<uint32_t, SRGB_TO_LINEAR>(
            (const uint32_t*)imageData, numSppIn, floatData, numSppOut, size, hasAlpha, priority, scale
        );
    } else if (kind == ETiffKind::Palette) {
        if (palette[0] == nullptr || palette[1] == nullptr || palette[2] == nullptr) {
            throw runtime_error{"Palette data is null."};
        }

        if (numSppOut < 3) {
            throw runtime_error{"Number of output samples per pixel must be at least 3 for palette data."};
        }

        size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numSppOut,
            [&](size_t i) {
                const uint32_t index = imageData[i * numSppIn];
                const float paletteScale = 1.0f / 65535.0f;
                for (size_t c = 0; c < 3; ++c) {
                    floatData[i * numSppOut + c] = palette[c][index] * paletteScale;
                }

                size_t numChannels = std::min(numSppOut, numSppIn + 2);
                for (size_t c = 3; c < numChannels; ++c) {
                    floatData[i * numSppOut + c] = imageData[i * numSppIn + c - 2] * scale;
                }
            },
            priority
        );
    } else {
        throw ImageLoadError{fmt::format("Unsupported sample format: {}", toString(kind))};
    }

    if (flipWhiteAndBlack) {
        size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numSppOut,
            [&](size_t i) {
                for (size_t c = 0; c < numSppOut; ++c) {
                    floatData[i * numSppOut + c] = 1.0f - floatData[i * numSppOut + c];
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
    tlog::error() << fmt::format("TIFF error ({}): {}", module ? module : "unknown", buffer);
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
    auto tiffData = reinterpret_cast<TiffData*>(handle);
    size = std::min(size, tiffData->size - (tsize_t)tiffData->offset);
    memcpy(data, tiffData->data + tiffData->offset, size);
    tiffData->offset += size;
    return size;
}

static tsize_t tiffWriteProc(thandle_t, tdata_t, tsize_t) {
    return 0; // We don't need to write
}

static toff_t tiffSeekProc(thandle_t handle, toff_t offset, int whence) {
    auto tiffData = reinterpret_cast<TiffData*>(handle);

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
    auto tiffData = reinterpret_cast<TiffData*>(handle);
    return tiffData->size;
}

static int tiffMapProc(thandle_t handle, tdata_t* pdata, toff_t* psize) {
    // We're not actually using memory mapping -- merely passing a pointer to the in-memory file data.
    auto tiffData = reinterpret_cast<TiffData*>(handle);
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

    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_ACTIVEAREA, TIFF_ANY)) {
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT:
                if (uint16_t* aa; TIFFGetField(tif, TIFFTAG_ACTIVEAREA, &aa)) {
                    activeArea.min.y() = aa[0];
                    activeArea.min.x() = aa[1];
                    activeArea.max.y() = aa[2];
                    activeArea.max.x() = aa[3];
                }
                break;
            case TIFF_LONG:
                if (uint32_t* aa; TIFFGetField(tif, TIFFTAG_ACTIVEAREA, &aa)) {
                    activeArea.min.y() = aa[0];
                    activeArea.min.x() = aa[1];
                    activeArea.max.y() = aa[2];
                    activeArea.max.x() = aa[3];
                }
                break;
            default: throw ImageLoadError{"Unsupported active area data type."};
        }
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

Task<void> demosaicCfa(TIFF* tif, span<float> cfaData, span<float> rgbData, const Vector2i size, int priority) {
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

        uint32_t numRead = 0;
        if (float* asn; TIFFGetField(tif, TIFFTAG_ASSHOTNEUTRAL, &numRead, &asn) && asn && numRead >= 3) {
            const float maxVal = std::max({asn[0], asn[1], asn[2]});
            wbScale = Vector3f{asn[0], asn[1], asn[2]} / maxVal;
            tlog::debug() << fmt::format("Clipping integer CFA to neutral white {}", wbScale);
        }
    }

    uint16_t* dim = nullptr;
    if (!TIFFGetField(tif, TIFFTAG_EP_CFAREPEATPATTERNDIM, &dim) || !dim) {
        throw ImageLoadError{"Missing or invalid CFARepeatPatternDim"};
    }

    const auto cfaSize = Vector2i{dim[1], dim[0]};
    const size_t patternSize = (size_t)cfaSize.x() * cfaSize.y();

    uint8_t* pat = nullptr;
    if (uint16_t readCount; !TIFFGetField(tif, TIFFTAG_EP_CFAPATTERN, &readCount, &pat) || !pat || readCount != patternSize) {
        throw ImageLoadError{"Missing or invalid CFAPattern"};
    }

    enum class ELayout : uint16_t {
        Rect = 1,
        A,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
    };
    ELayout layout = ELayout::Rect;

    if (TIFFGetField(tif, TIFFTAG_CFALAYOUT, (uint16_t*)&layout) && layout != ELayout::Rect) {
        tlog::warning() << fmt::format("Found CFALayout tag with non-rectangular value {}; not supported yet", (uint16_t)layout);
    }

    tlog::debug() << fmt::format("Found CFA pattern of size {}; applying...", cfaSize);

    const auto numPixels = (size_t)size.x() * size.y();

    if (wbScale != Vector3f{1.0f}) {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels,
            [&](int y) {
                const size_t offset = (size_t)y * size.x();
                for (int x = 0; x < size.x(); ++x) {
                    cfaData[offset + x] =
                        std::clamp(cfaData[offset + x] / wbScale[pat[(y % cfaSize.y()) * cfaSize.x() + (x % cfaSize.x())]], 0.0f, 1.0f);
                }
            },
            priority
        );
    }

    co_await demosaic(cfaData, rgbData, size, span<const uint8_t>(pat, patternSize), cfaSize, priority);

    if (wbScale != Vector3f{1.0f}) {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels,
            [&](int y) {
                const size_t offset = (size_t)y * size.x();
                for (int x = 0; x < size.x(); ++x) {
                    const size_t idx = (offset + x) * 3;
                    for (int c = 0; c < 3; ++c) {
                        rgbData[idx + c] = rgbData[idx + c] * wbScale[c];
                    }
                }
            },
            priority
        );
    }
}

Task<void> linearizeAndNormalizeRawDng(
    TIFF* tif,
    const uint16_t dataSampleFormat,
    const uint16_t dataBitsPerSample,
    const uint16_t samplesPerPixel,
    const int numColorChannels,
    const int numRgbaChannels,
    span<float> floatRgbaData,
    const Vector2i size,
    const int priority
) {
    const auto numPixels = (size_t)size.x() * size.y();

    // Utility var that we'll reuse whenever reading a variable TIFF array
    uint32_t numRead = 0;
    const size_t maxVal = (1ull << dataBitsPerSample) - 1;
    const float scale = 1.0f / maxVal;

    // 1. Map colors via linearization table if it exists and the data is not already float
    if (uint16_t* linTable; TIFFGetField(tif, TIFFTAG_LINEARIZATIONTABLE, &numRead, &linTable) && linTable && numRead >= 2) {
        tlog::debug() << fmt::format("Found linearization table of size {}; applying...", numRead);

        if (dataSampleFormat == SAMPLEFORMAT_IEEEFP) {
            tlog::warning() << "Data is already in floating point format, but a linearization table is present. Skipping.";
        } else {
            const size_t maxIdx = numRead - 1;

            co_await ThreadPool::global().parallelForAsync<size_t>(
                0,
                numPixels,
                numPixels * numColorChannels,
                [&](size_t i) {
                    for (int c = 0; c < numColorChannels; ++c) {
                        const float val = floatRgbaData[i * numRgbaChannels + c];

                        // Lerp the transfer function
                        const size_t idx = clamp((size_t)(val * maxVal), (size_t)0, maxIdx - 1);
                        const float w = val * maxIdx - idx;
                        floatRgbaData[i * numRgbaChannels + c] = (1.0f - w) * linTable[idx] * scale + w * linTable[idx + 1] * scale;
                    }
                },
                priority
            );
        }
    }

    // 2. Subtract black level
    vector<float> maxBlackLevel(samplesPerPixel, 0.0f);
    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_BLACKLEVEL, TIFF_ANY)) {
        uint16_t blackLevelRepeatRows = 1;
        uint16_t blackLevelRepeatCols = 1;
        if (const uint16_t* blackLevelRepeatDim; TIFFGetField(tif, TIFFTAG_BLACKLEVELREPEATDIM, &blackLevelRepeatDim)) {
            blackLevelRepeatRows = blackLevelRepeatDim[0];
            blackLevelRepeatCols = blackLevelRepeatDim[1];
        }

        tlog::debug() << fmt::format("Found {}x{} black level data; applying...", blackLevelRepeatRows, blackLevelRepeatCols);

        const size_t numBlackLevelPixels = blackLevelRepeatRows * blackLevelRepeatCols;

        vector<float> blackLevel(numBlackLevelPixels * samplesPerPixel, 0.0f);
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT:
                if (uint16_t* blackLevelShort; TIFFGetField(tif, TIFFTAG_BLACKLEVEL, &numRead, &blackLevelShort)) {
                    if (numRead != blackLevel.size()) {
                        throw ImageLoadError{
                            fmt::format("Invalid number of short black level pixels: expected {}, got {}", blackLevel.size(), numRead)
                        };
                    }

                    for (size_t i = 0; i < blackLevel.size(); ++i) {
                        blackLevel[i] = blackLevelShort[i] * scale;
                        tlog::debug() << fmt::format("Black short level[{}] = {}", i, blackLevel[i]);
                    }
                } else {
                    throw ImageLoadError{"Failed to read short black level data."};
                }
                break;
            case TIFF_LONG:
                if (uint32_t* blackLevelLong; TIFFGetField(tif, TIFFTAG_BLACKLEVEL, &numRead, &blackLevelLong)) {
                    if (numRead != blackLevel.size()) {
                        throw ImageLoadError{
                            fmt::format("Invalid number of long black level pixels: expected {}, got {}", blackLevel.size(), numRead)
                        };
                    }

                    for (size_t i = 0; i < blackLevel.size(); ++i) {
                        blackLevel[i] = blackLevelLong[i] * scale;
                        tlog::debug() << fmt::format("Black long level[{}] = {}", i, blackLevel[i]);
                    }
                } else {
                    throw ImageLoadError{"Failed to read short black level data."};
                }
                break;
            case TIFF_RATIONAL:
                if (float* blackLevelFloat; TIFFGetField(tif, TIFFTAG_BLACKLEVEL, &numRead, &blackLevelFloat)) {
                    if (numRead != blackLevel.size()) {
                        throw ImageLoadError{
                            fmt::format("Invalid number of rational black level pixels: expected {}, got {}", blackLevel.size(), numRead)
                        };
                    }

                    for (size_t i = 0; i < blackLevel.size(); ++i) {
                        blackLevel[i] = blackLevelFloat[i] * scale;
                        tlog::debug() << fmt::format("Black rational level[{}] = {}", i, blackLevel[i]);
                    }
                } else {
                    throw ImageLoadError{"Failed to read short black level data."};
                }
                break;
            default: throw ImageLoadError{fmt::format("Unsupported black level data type: {}", (uint32_t)TIFFFieldDataType(field))};
        }

        vector<float> blackLevelDeltaH(size.x(), 0.0f);
        vector<float> blackLevelDeltaV(size.y(), 0.0f);

        if (float* bldh; TIFFGetField(tif, TIFFTAG_BLACKLEVELDELTAH, &numRead, &bldh)) {
            tlog::debug() << fmt::format("Found {} black level H entries", numRead);
            if (numRead != blackLevelDeltaH.size()) {
                throw ImageLoadError{"Invalid number of black level delta H pixels."};
            }

            for (size_t i = 0; i < blackLevelDeltaH.size(); ++i) {
                blackLevelDeltaH[i] = bldh[i] * scale;
            }
        }

        if (float* bldv; TIFFGetField(tif, TIFFTAG_BLACKLEVELDELTAV, &numRead, &bldv)) {
            tlog::debug() << fmt::format("Found {} black level V entries", numRead);
            if (numRead != blackLevelDeltaV.size()) {
                throw ImageLoadError{"Invalid number of black level delta V pixels."};
            }

            for (size_t i = 0; i < blackLevelDeltaV.size(); ++i) {
                blackLevelDeltaV[i] = bldv[i] * scale;
            }
        }

        vector<float> maxBlackLevelY(samplesPerPixel * size.y(), 0.0f);
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels * numColorChannels,
            [&](const int y) {
                const float deltaV = blackLevelDeltaV[y];
                for (int x = 0; x < size.x(); ++x) {
                    const float deltaH = blackLevelDeltaH[x];
                    const float delta = deltaH + deltaV;

                    const size_t idx = (size_t)y * size.x() + x;
                    const size_t blIdx = (y % blackLevelRepeatRows) * blackLevelRepeatCols + (x % blackLevelRepeatCols);

                    for (int c = 0; c < numColorChannels; ++c) {
                        const float bl = blackLevel[blIdx * samplesPerPixel + c] + delta;
                        floatRgbaData[idx * numRgbaChannels + c] -= bl;
                        maxBlackLevelY[y * samplesPerPixel + c] = std::max(maxBlackLevelY[y * samplesPerPixel + c], bl);
                    }
                }
            },
            priority
        );

        maxBlackLevel.assign(samplesPerPixel, numeric_limits<float>::lowest());
        for (int y = 0; y < size.y(); ++y) {
            for (int c = 0; c < numColorChannels; ++c) {
                maxBlackLevel[c] = std::max(maxBlackLevel[c], maxBlackLevelY[y * samplesPerPixel + c]);
            }
        }
    }

    // 3. Rescale to 0-1 range per white level
    vector<float> whiteLevel(samplesPerPixel, 1.0f);
    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_WHITELEVEL, TIFFDataType::TIFF_ANY)) {
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT:
                if (uint16_t* whiteLevelShort; TIFFGetField(tif, TIFFTAG_WHITELEVEL, &numRead, &whiteLevelShort)) {
                    if (numRead != whiteLevel.size()) {
                        throw ImageLoadError{
                            fmt::format("Invalid number of short white level pixels: expected {}, got {}", whiteLevel.size(), numRead)
                        };
                    }

                    for (size_t i = 0; i < whiteLevel.size(); ++i) {
                        whiteLevel[i] = whiteLevelShort[i] * scale;
                    }
                } else {
                    throw ImageLoadError{"Failed to read short white level data."};
                }
                break;
            case TIFF_LONG:
                if (uint32_t* whiteLevelLong; TIFFGetField(tif, TIFFTAG_WHITELEVEL, &numRead, &whiteLevelLong)) {
                    if (numRead != whiteLevel.size()) {
                        throw ImageLoadError{
                            fmt::format("Invalid number of long white level pixels: expected {}, got {}", whiteLevel.size(), numRead)
                        };
                    }

                    for (size_t i = 0; i < whiteLevel.size(); ++i) {
                        whiteLevel[i] = whiteLevelLong[i] * scale;
                    }
                } else {
                    throw ImageLoadError{"Failed to read short white level data."};
                }
                break;
            default: throw ImageLoadError{fmt::format("Unsupported white level data type: {}", (uint32_t)TIFFFieldDataType(field))};
        }

        tlog::debug() << "Found white level data";
    }

    vector<float> channelScale(samplesPerPixel);
    for (int c = 0; c < samplesPerPixel; ++c) {
        tlog::debug() << fmt::format("whiteLevel[{}] = {}", c, whiteLevel[c]);
        tlog::debug() << fmt::format("maxBlackLevel[{}] = {}", c, maxBlackLevel[c]);
        channelScale[c] = 1.0f / (whiteLevel[c] - maxBlackLevel[c]);
    }

    if (any_of(channelScale.begin(), channelScale.end(), [](float s) { return s != 1.0f; })) {
        tlog::debug() << fmt::format("Non-1.0 channel scale [{}]", join(channelScale, ","));

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    floatRgbaData[i * numRgbaChannels + c] *= channelScale[c];
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
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    floatRgbaData[i * numRgbaChannels + c] = std::min(floatRgbaData[i * numRgbaChannels + c], 1.0f);
                }
            },
            priority
        );
    }
}

Task<void> postprocessLinearRawDng(
    TIFF* tif,
    const uint16_t samplesPerPixel,
    const int numColorChannels,
    const int numRgbaChannels,
    span<float> floatRgbaData,
    ImageData& resultData,
    const bool reverseEndian,
    const int priority
) {
    if (samplesPerPixel != 3) {
        throw ImageLoadError{"Linear RAW images with sampledPerPixel != 3 are not supported."};
    }

    // We follow page 96 of https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_0_0.pdf
    tlog::debug() << "Mapping LinearRAW to linear RGB...";

    const auto size = resultData.size();
    const auto numPixels = (size_t)size.x() * size.y();

    // Utility var that we'll reuse whenever reading a variable TIFF array
    uint32_t numRead = 0;

    // Camera parameters are stored in IFD 0, so let's switch to it temporarily.
    const uint64_t prevOffset = TIFFCurrentDirOffset(tif);
    TIFFSetDirectory(tif, 0);
    const ScopeGuard guard{[&]() { TIFFSetSubDirectory(tif, prevOffset); }};

    Vector3f analogBalance{1.0f};
    if (float* abt; TIFFGetField(tif, TIFFTAG_ANALOGBALANCE, &numRead, &abt)) {
        if (numRead != samplesPerPixel) {
            throw ImageLoadError{"Invalid number of analog balance pixels."};
        }

        for (size_t i = 0; i < numRead; ++i) {
            analogBalance[i] = abt[i];
        }

        tlog::debug() << fmt::format("Analog balance: {}", analogBalance);
    }

    const auto readCameraToXyz = [&](int CCTAG, int CMTAG, int CALTAG) {
        Matrix3f colorMatrix{1.0f};
        if (float* cmt; TIFFGetField(tif, CMTAG, &numRead, &cmt)) {
            if (numRead != (uint32_t)samplesPerPixel * samplesPerPixel) {
                throw ImageLoadError{"Invalid number of camera matrix entries."};
            }

            for (size_t i = 0; i < samplesPerPixel; ++i) {
                for (size_t j = 0; j < samplesPerPixel; ++j) {
                    colorMatrix.m[j][i] = cmt[i * samplesPerPixel + j];
                }
            }

            tlog::debug() << fmt::format("Found color matrix: {}", colorMatrix);
        } else {
            return optional<Matrix3f>{};
        }

        Matrix3f cameraCalibration{1.0f};
        if (float* cct; TIFFGetField(tif, CCTAG, &numRead, &cct)) {
            if (numRead != (uint32_t)samplesPerPixel * samplesPerPixel) {
                throw ImageLoadError{"Invalid number of camera calibration entries."};
            }

            for (size_t i = 0; i < samplesPerPixel; ++i) {
                for (size_t j = 0; j < samplesPerPixel; ++j) {
                    cameraCalibration.m[j][i] = cct[i * samplesPerPixel + j];
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
        if (float* asn; TIFFGetField(tif, TIFFTAG_ASSHOTNEUTRAL, &numRead, &asn) && asn && numRead >= samplesPerPixel) {
            if (numRead != samplesPerPixel) {
                throw ImageLoadError{"Invalid number of AsShotNeutral entries."};
            }

            const Vector3f asShotNeutral{asn[0], asn[1], asn[2]};
            tlog::debug() << fmt::format("Adapting white to D50 based on AsShotNeutral={}", asShotNeutral);

            const Vector3f xyz = cameraToXyz * asShotNeutral;
            const float sxyz = xyz.x() + xyz.y() + xyz.z();
            const Vector2f xy = {xyz.x() / sxyz, xyz.y() / sxyz};

            chromaticAdaptation = adaptWhiteBradford(xy, whiteD50());
        } else if (float* aswp; TIFFGetField(tif, TIFFTAG_ASSHOTWHITEXY, &numRead, &aswp) && aswp && numRead >= 2) {
            if (numRead != samplesPerPixel) {
                throw ImageLoadError{"Invalid number of AsShotNeutral entries."};
            }

            const Vector2f cameraNeutralXy = {aswp[0], aswp[1]};

            tlog::debug() << fmt::format("Adapting white to D50 based on AsShotWhiteXY={}", cameraNeutralXy);
            chromaticAdaptation = adaptWhiteBradford(cameraNeutralXy, whiteD50());
        } else if (uint16_t illu; adaptToExifIlluminant && TIFFGetField(tif, CALTAG, &illu)) {
            EExifLightSource illuminant = static_cast<EExifLightSource>(illu);
            tlog::debug() << fmt::format("Found illuminant={}/{}", toString(illuminant), illu);

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

    // If present, matrix 3 represents the illuminant used to capture the image. If not, we use the illuminant from matrix 2 which
    // is supposed to be the colder one (closer to D65). Once a matrix is selected, we construct a transformation to ProPhoto RGB
    // (aka RIMM space) in which we will apply the camera profile.
    auto toRimm = xyzToChromaMatrix(proPhotoChroma());
    for (size_t i = 0; i < camTags.size(); ++i) {
        if (const auto camToXyz = readCameraToXyz(get<0>(camTags[i]), get<1>(camTags[i]), get<2>(camTags[i]))) {
            tlog::debug() << fmt::format("Applying camToXyz matrix #{}: {}", camTags.size() - i, camToXyz.value());
            toRimm = toRimm * camToXyz.value();
            break;
        }
    }

    // The remaining camera profile transformation is applied in linear ProPhoto RGB space (aka RIMM space)
    span<Vector3f> rgbData{reinterpret_cast<Vector3f*>(floatRgbaData.data()), (size_t)size.x() * size.y()};
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0, numPixels, numPixels * numColorChannels, [&](size_t i) { rgbData[i] = toRimm * rgbData[i]; }, priority
    );

    // Once we're done, we want to convert from RIMM space to sRGB/Rec709. We actually have a choice here whether we want to stay in
    // scene-referred coordinates and set downstream processing to be absolute, or to convert to display-referred coordinates right away.
    // For now, we choose the latter (to match libraw's behavior and to tev's DNG display look closer to the JPG preview).
    resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;
    resultData.toRec709 = convertColorspaceMatrix(proPhotoChroma(), rec709Chroma(), resultData.renderingIntent);

    struct ProfileDynamicRange {
        uint16_t version, dynamicRange;
        float hintMaxOutputValue;
    };

    bool isHdr = 0;
    static constexpr uint16_t TIFFTAG_PROFILEDYNAMICRANGE = 52551;
    if (ProfileDynamicRange* pdr; TIFFGetField(tif, TIFFTAG_PROFILEDYNAMICRANGE, &pdr)) {
        if (reverseEndian) {
            pdr->version = swapBytes(pdr->version);
            pdr->dynamicRange = swapBytes(pdr->dynamicRange);
        }

        tlog::debug() << fmt::format(
            "Found profile dynamic range: version={} dynamicRange={} hintMaxOutputValue={}", pdr->version, pdr->dynamicRange, pdr->hintMaxOutputValue
        );

        // Per DNG 1.7.0.0, page 93, a value of 1 refers to HDR images that need to be compressed into 0-1 before the following transforms
        // take place.
        isHdr = pdr->dynamicRange == 1;
    }

    // NOTE: The order of the following operations is defined on pages 71/72 of the DNG spec.
    float exposureScale = 1.0f;
    if (float baselineExposure; TIFFGetField(tif, TIFFTAG_BASELINEEXPOSURE, &baselineExposure)) {
        tlog::debug() << fmt::format("Baseline exposure: {}", baselineExposure);
        exposureScale *= exp2f(baselineExposure);
    }

    if (float baselineExposureOffset; TIFFGetField(tif, TIFFTAG_BASELINEEXPOSUREOFFSET, &baselineExposureOffset)) {
        tlog::debug() << fmt::format("Baseline exposure offset: {}", baselineExposureOffset);
        exposureScale *= exp2f(baselineExposureOffset);
    }

    if (exposureScale != 1.0f) {
        tlog::debug() << fmt::format("Applying exposure scale: {}", exposureScale);
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    floatRgbaData[i * numRgbaChannels + c] *= exposureScale;
                }
            },
            priority
        );
    }

    // At this point, we have the image in linear ProPhoto RGB space. This is most faithful to the readings from the sensor *in theory*, but
    // the camera may have embedded a (potentially user-chosen) color profile that, per the DNG spec, can be used as a starting point for
    // further user editing. *In practice*, DNGs from some sources, e.g. iPhone, seem SDR before applying the profile and proper HDR after
    // applying it, so we do by default.
    const bool applyCameraProfile = true;
    if (!applyCameraProfile) {
        co_return;
    }

    if (const char* str; TIFFGetField(tif, TIFFTAG_PROFILENAME, &numRead, &str)) {
        tlog::debug() << fmt::format("Applying camera profile \"{}\"", str);
    }

    {
        // Temporarily switch back to the raw's IFD to read gain table map, if present.
        TIFFSetSubDirectory(tif, prevOffset);
        ScopeGuard guard2{[&]() { TIFFSetDirectory(tif, 0); }};

        uint32_t numReadGainTableMap = 0;
        if (const uint8_t* gainTableMap; TIFFGetField(tif, TIFFTAG_PROFILEGAINTABLEMAP, &numReadGainTableMap, &gainTableMap)) {
            // TODO: implement profile gain table map
            tlog::warning() << "Found gain table map, but not implemented yet. Color profile may look wrong.";
        }
    }

    // Profile application has to happen in SDR space if the image is HDR
    if (isHdr) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    const size_t idx = i * numRgbaChannels + c;
                    floatRgbaData[idx] = dngHdrEncodingFunction(floatRgbaData[idx]);
                }
            },
            priority
        );
    }

    if (const uint32_t* dims; TIFFGetField(tif, TIFFTAG_PROFILEHUESATMAPDIMS, &dims)) {
        uint32_t hueDivisions = dims[0];
        uint32_t satDivisions = dims[1];
        uint32_t valueDivisions = dims[2];

        tlog::debug() << fmt::format("Hue/sat/val map dimensions: {}x{}x{}", hueDivisions, satDivisions, valueDivisions);

        // TODO: implement hue/sat/val map...
        tlog::warning() << "Found hue/sat/val map, but not implemented yet. Color profile may look wrong.";
    }

    if (const uint32_t* dims; TIFFGetField(tif, TIFFTAG_PROFILELOOKTABLEDIMS, &dims)) {
        uint32_t hueDivisions = dims[0];
        uint32_t satDivisions = dims[1];
        uint32_t valueDivisions = dims[2];

        tlog::debug() << fmt::format("Look table dimensions: {}x{}x{}", hueDivisions, satDivisions, valueDivisions);

        // TODO: implement hue/sat/val map...
        tlog::warning() << "Found look table, but not implemented yet. Color profile may look wrong.";
    }

    if (const float* tonecurve; TIFFGetField(tif, TIFFTAG_PROFILETONECURVE, &numRead, &tonecurve)) {
        if (numRead % 2 != 0 || numRead < 4) {
            throw ImageLoadError{"Number of tone curve entries must be divisible by 2 and >=4."};
        }

        tlog::debug() << fmt::format("Applying profile tone curve of length {}", numRead);

        span<const Vector2f> tc{reinterpret_cast<const Vector2f*>(tonecurve), numRead / 2};
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
            numPixels * numColorChannels * 16, // arbitrary factor to estimate pw linear cost
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    const size_t idx = i * numRgbaChannels + c;
                    floatRgbaData[idx] = applyPwLinear(tc, floatRgbaData[idx]);
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
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    const size_t idx = i * numRgbaChannels + c;
                    floatRgbaData[idx] = dngHdrDecodingFunction(floatRgbaData[idx]);
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
    const int numColorChannels,
    const int numRgbaChannels,
    span<float> floatRgbaData,
    ImageData& resultData,
    const int priority
) {
    const Vector2i size = resultData.size();

    chroma_t chroma = rec709Chroma();
    if (float* primaries; TIFFGetField(tif, TIFFTAG_PRIMARYCHROMATICITIES, &primaries)) {
        tlog::debug() << "Found custom primaries; applying...";
        chroma[0] = {primaries[0], primaries[1]};
        chroma[1] = {primaries[2], primaries[3]};
        chroma[2] = {primaries[4], primaries[5]};
    }

    if (float* whitePoint; TIFFGetField(tif, TIFFTAG_WHITEPOINT, &whitePoint)) {
        tlog::debug() << "Found custom white point; applying...";
        chroma[3] = {whitePoint[0], whitePoint[1]};
    }

    // Assume the RGB TIFF image is display-referred and not scene-referred, so we'll adapt the white point. While scene-referred linear
    // TIFF images *do* exist in the wild, there is, unfortunately, no unambiguous way to determine this from the TIFF metadata alone.
    resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;
    resultData.toRec709 = convertColorspaceMatrix(chroma, rec709Chroma(), resultData.renderingIntent);
    resultData.nativeMetadata.chroma = chroma;

    enum EPreviewColorSpace : uint32_t { Unknown = 0, Gamma2_2 = 1, sRGB = 2, AdobeRGB = 3, ProPhotoRGB = 4 };

    if (uint16_t* transferFunction[3];
        TIFFGetField(tif, TIFFTAG_TRANSFERFUNCTION, &transferFunction[0], &transferFunction[1], &transferFunction[2])) {
        // In TIFF, transfer functions are stored as 2**bitsPerSample values in the range [0, 65535] per color channel. The transfer
        // function is a linear interpolation between these values.
        tlog::debug() << "Found custom transfer function; applying...";

        const float scale = 1.0f / 65535.0f;

        const size_t bps = photometric == PHOTOMETRIC_PALETTE ? 16 : dataBitsPerSample;
        const size_t maxIdx = (1ull << bps) - 1;

        const size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    float val = floatRgbaData[i * numRgbaChannels + c];

                    // Lerp the transfer function
                    size_t idx = clamp((size_t)(val * maxIdx), (size_t)0, maxIdx - 1);
                    float w = val * maxIdx - idx;
                    floatRgbaData[i * numRgbaChannels + c] = (1.0f - w) * transferFunction[c][idx] * scale +
                        w * transferFunction[c][idx + 1] * scale;
                }
            },
            priority
        );

        resultData.nativeMetadata.transfer = ituth273::ETransfer::LUT;
    } else if (uint32_t pcsInt; TIFFGetField(tif, TIFFTAG_PREVIEWCOLORSPACE, &pcsInt) && pcsInt > 1) {
        // Alternatively, if we're a preview image from a DNG file, we can use the preview color space to determine the transfer. Values
        // 0 (Unknown) and 1 (Gamma 2.2) are handled by the following `else` block. Other values are handled in this one.
        tlog::debug() << fmt::format("Found preview color space: {}", (uint32_t)pcsInt);

        const EPreviewColorSpace pcs = static_cast<EPreviewColorSpace>(pcsInt);

        size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    float v = floatRgbaData[i * numRgbaChannels + c];
                    floatRgbaData[i * numRgbaChannels + c] = toLinear(v);
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
                for (int c = 0; c < numColorChannels; ++c) {
                    // We use the absolute value here to avoid having to clamp negative values to 0 -- we instead pretend that
                    // the power behaves like an odd exponent, thereby preserving the range of R.
                    float v = floatRgbaData[i * numRgbaChannels + c];
                    floatRgbaData[i * numRgbaChannels + c] = copysign(pow(abs(v), 2.2f), v);
                }
            },
            priority
        );

        resultData.nativeMetadata.transfer = ituth273::ETransfer::Gamma22;
    }
}

Task<ImageData>
    readTiffImage(TIFF* tif, const bool isDng, const bool shallDemosaic, const bool reverseEndian, string_view partName, const int priority) {
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

    // DNG's lossy JPEG compression can be decoded by libtiff's regular JPEG decoder
    static const uint16_t COMPRESSION_LOSSY_JPEG = 34892;
    if (compression == COMPRESSION_LOSSY_JPEG) {
        if (!TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG)) {
            throw ImageLoadError{"Failed to set LossyJPEG->JPEG."};
        }

        compression = COMPRESSION_JPEG;
    }

    uint16_t photometric;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric)) {
        throw ImageLoadError{"Failed to read photometric interpretation."};
    }

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

    // We will manually decompress JXL and JPEG2000 tiles further down the pipeline by invoking tev's JXL decoder directly on the compressed
    // data from the TIFF file. This returns fp32 data.
    if (compression == COMPRESSION_JXL_DNG_1_7 || compression == COMPRESSION_JXL || compression == COMPRESSION_JP2000) {
        bitsPerSample = 32;
        sampleFormat = SAMPLEFORMAT_IEEEFP;
    }

    if (compression == COMPRESSION_JPEG) {
        // For JPEG decoding, we need to pretend to have more bits per sample than is in the data for the JPEG decoder to work correctly.
        // This is largely fine for the following decoding steps, but we will later need to pass `dataBitsPerSample` to postprocessing for
        // scaling purposes.
        if (bitsPerSample <= 8) {
            bitsPerSample = 8;
        } else if (bitsPerSample <= 12) {
            bitsPerSample = 12;
        } else if (bitsPerSample <= 16) {
            bitsPerSample = 16;
        }

        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bitsPerSample);

        if (photometric == PHOTOMETRIC_YCBCR) {
            tlog::debug() << "Converting JPEG YCbCr to RGB.";

            photometric = PHOTOMETRIC_RGB;
        }

        // Always set the JPEG color mode, even if it already is RGB, to retrigger libtiff's internal tile size calcs after bitsPerSample
        // changed.
        if (!TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB)) {
            throw ImageLoadError{"Failed to set JPEG color mode."};
        }
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
        throw ImageLoadError{"YCbCr images are unsupported."};
    }

    // TODO: handle CIELAB, ICCLAB, ITULAB (shouldn't be too tough)

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
        uint32_t numRead = 0;
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT:
                if (uint16_t* f; TIFFGetField(tif, TIFFTAG_COLINTERLEAVEFACTOR, &numRead, &f) && f && numRead >= 1) {
                    interleave.x() = *f;
                }
                break;
            case TIFF_LONG:
                if (uint32_t* f; TIFFGetField(tif, TIFFTAG_COLINTERLEAVEFACTOR, &numRead, &f) && f && numRead >= 1) {
                    interleave.x() = (int)*f;
                }
                break;
            default: throw ImageLoadError{"Unsupported col interleave factor type."};
        }
    }

    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_ROWINTERLEAVEFACTOR, TIFF_ANY)) {
        uint32_t numRead = 0;
        switch (TIFFFieldDataType(field)) {
            case TIFF_SHORT:
                if (uint16_t* f; TIFFGetField(tif, TIFFTAG_ROWINTERLEAVEFACTOR, &numRead, &f) && f && numRead >= 1) {
                    interleave.y() = *f;
                }
                break;
            case TIFF_LONG:
                if (uint32_t* f; TIFFGetField(tif, TIFFTAG_ROWINTERLEAVEFACTOR, &numRead, &f) && f && numRead >= 1) {
                    interleave.y() = (int)*f;
                }
                break;
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
    uint16_t* extraChannelTypes = nullptr;
    uint16_t numExtraChannels = 0;

    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &numExtraChannels, &extraChannelTypes)) {
        for (uint16_t i = 0; i < numExtraChannels; ++i) {
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

    const uint16_t* palette[3] = {};
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

        if (!TIFFGetField(tif, TIFFTAG_COLORMAP, &palette[0], &palette[1], &palette[2])) {
            throw ImageLoadError{"Failed to read color palette."};
        }
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
    const auto readTile = isTiled ? TIFFReadEncodedTile : TIFFReadEncodedStrip;

    const auto readRawTile = isTiled ? TIFFReadRawTile : TIFFReadRawStrip;
    const auto getRawTileSize = [isTiled](TIFF* tif, uint32_t tileIndex) -> size_t {
        const uint64_t* rawTileSize = NULL;
        if (!TIFFGetField(tif, isTiled ? TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS, &rawTileSize) || !rawTileSize) {
            throw ImageLoadError{fmt::format("Failed to read raw tile size for tile {}", tileIndex)};
        }

        return (size_t)rawTileSize[tileIndex];
    };

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

    // Be robust against broken TIFFs that have a tile/strip size smaller than the actual data size. Make sure to allocate enough memory to
    // fit all data.
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

    const bool decodeRaw = compression == COMPRESSION_JXL_DNG_1_7 || compression == COMPRESSION_JXL;

    vector<Task<void>> decodeTasks;

    // Read tiled/striped data. Unfortunately, libtiff doesn't support reading all tiles/strips in parallel, so we have to do that
    // sequentially.
    HeapArray<uint8_t> imageData((size_t)size.x() * size.y() * samplesPerPixel * unpackedBitsPerSample / 8);
    for (size_t i = 0; i < tile.count; ++i) {
        uint8_t* const td = tileData.data() + tile.size * i;

        if (decodeRaw) {
            const size_t rawTileSize = getRawTileSize(tif, (uint32_t)i);
            if (rawTileSize == 0) {
                co_await awaitAll(decodeTasks);
                throw ImageLoadError{fmt::format("Raw tile size is 0 for tile {}", i)};
            }

            HeapArray<uint8_t> compressedTileData(rawTileSize);
            if (readRawTile(tif, (uint32_t)i, compressedTileData.data(), rawTileSize) < 0) {
                co_await awaitAll(decodeTasks);
                throw ImageLoadError{fmt::format("Failed to read raw tile {}", i)};
            }

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
                                    compressedTileData, "", "", priority, {}, false, &nestedBitsPerSample, &nestedPixelType
                                );
                            } break;
                            case COMPRESSION_JP2000: {
                                const auto loader = Jpeg2000ImageLoader{};
                                tmp = co_await loader.load(
                                    compressedTileData, "", "", priority, {}, false, &nestedBitsPerSample, &nestedPixelType
                                );
                            } break;
                            default: throw ImageLoadError{fmt::format("Unsupported compression type: {}", compression)};
                        }

                        if (tmp.size() != 1) {
                            throw ImageLoadError{fmt::format("Expected exactly one image from tile, got {}", tmp.size())};
                        }

                        const auto& tmpImage = tmp.front();

                        if (tmpImage.channels.size() < (size_t)samplesPerPixel / numPlanes) {
                            throw ImageLoadError{
                                fmt::format("Tile has too few channels: expected {}, got {}", numPlanes, tmpImage.channels.size())
                            };
                        }

                        for (const auto& channel : tmpImage.channels) {
                            const auto tileSize = Vector2i{(int)tile.width, (int)tile.height};
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

                        const size_t numPixels = (size_t)tile.width * tile.height;

                        auto* const data = (float*)imageData.data();

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
                                            const auto pixel = tmpImage.channels[c].at({x0, y0});
                                            data[(y * size.x() + x) * samplesPerPixel + c] = pixel * scale;
                                        }
                                    }
                                } else {
                                    size_t c = i / numTilesPerPlane;
                                    for (int x = xStart; x < xEnd; ++x) {
                                        const int x0 = x - xStart;
                                        const auto pixel = tmpImage.channels[0].at({x0, y0});
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

    if (const auto activeArea = getActiveArea(tif, size); size != activeArea.size()) {
        const auto rawSize = size;
        size = activeArea.size();

        tlog::debug() << fmt::format("Cropping to active area: [{},{}] -> {}", activeArea.min, activeArea.max, size);

        resultData.dataWindow = resultData.displayWindow = {size.x(), size.y()};

        HeapArray<uint8_t> croppedImageData((size_t)size.x() * size.y() * samplesPerPixel * unpackedBitsPerSample / 8);

        const auto cropTask = [&](auto* const croppedData, auto* const data) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                (size_t)size.x() * numChannels,
                [&](int y) {
                    const int srcY = y + activeArea.min.y();
                    for (int x = 0; x < size.x(); ++x) {
                        const int srcX = x + activeArea.min.x();
                        for (size_t c = 0; c < numChannels; ++c) {
                            croppedImageData[(y * size.x() + x) * numChannels + c] = imageData[(srcY * rawSize.x() + srcX) * numChannels + c];
                        }
                    }
                },
                priority
            );
        };

        if (unpackedBitsPerSample == 64) {
            co_await cropTask((uint64_t*)croppedImageData.data(), (const uint64_t*)imageData.data());
        } else if (unpackedBitsPerSample == 32) {
            co_await cropTask((uint32_t*)croppedImageData.data(), (const uint32_t*)imageData.data());
        } else {
            throw ImageLoadError{fmt::format("Unsupported unpacked bits per sample: {}", unpackedBitsPerSample)};
        }

        imageData = std::move(croppedImageData);
    }

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

    const bool flipWhiteAndBlack = photometric == PHOTOMETRIC_MINISWHITE;

    // Convert all the extra channels to float and store them in the result data. No further processing needed.
    for (size_t c = numChannels - numExtraChannels + (hasAlpha ? 1 : 0); c < numChannels; ++c) {
        co_await tiffDataToFloat32<false>(
            kind,
            interleave,
            nullptr,
            (uint32_t*)(imageData.data() + c * unpackedBitsPerSample / 8),
            numChannels,
            resultData.channels[c].floatData(),
            1,
            size,
            false,
            priority,
            intConversionScale,
            flipWhiteAndBlack
        );
    }

    // The RGBA channels might need color space conversion: store them in a staging buffer first and then try ICC conversion
    // Try color space conversion using ICC profile if available. This is going to be the most accurate method.
    if (iccProfileData && iccProfileSize > 0) {
        try {
            HeapArray<float> iccTmpFloatData(size.x() * (size_t)size.y() * numRgbaChannels);
            co_await tiffDataToFloat32<false>(
                kind,
                interleave,
                palette,
                (uint32_t*)imageData.data(),
                numChannels,
                iccTmpFloatData.data(),
                numRgbaChannels,
                size,
                hasAlpha,
                priority,
                intConversionScale,
                flipWhiteAndBlack
            );

            const auto profile = ColorProfile::fromIcc({(uint8_t*)iccProfileData, iccProfileSize});
            co_await toLinearSrgbPremul(
                profile,
                size,
                numColorChannels,
                hasAlpha ? (hasPremultipliedAlpha ? EAlphaKind::Premultiplied : EAlphaKind::Straight) : EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)iccTmpFloatData.data(),
                resultData.channels.front().floatData(),
                numInterleavedChannels,
                nullopt,
                priority
            );

            resultData.hasPremultipliedAlpha = true;
            resultData.readMetadataFromIcc(profile);

            co_return resultData;
        } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    // Write directly into the final RGBA buffer if possible to save one copy, otherwise use a staging buffer.
    HeapArray<float> floatRgbaDataBuffer;
    span<float> floatRgbaData;
    if (numRgbaChannels == numInterleavedChannels) {
        floatRgbaData = {resultData.channels.front().floatData(), (size_t)size.x() * size.y() * numRgbaChannels};
    } else {
        floatRgbaDataBuffer.resize((size_t)size.x() * size.y() * numRgbaChannels);
        floatRgbaData = floatRgbaDataBuffer;
    }

    co_await tiffDataToFloat32<false>(
        kind,
        interleave,
        palette,
        (uint32_t*)imageData.data(),
        numChannels,
        floatRgbaData.data(),
        numRgbaChannels,
        size,
        hasAlpha,
        priority,
        intConversionScale,
        flipWhiteAndBlack
    );

    // Both CFA and linear raw DNG data need to be linearized and normalized prior to color space conversions. Metadata for linearization
    // assumes *pre* demosaicing data, so this step needs to be done before we convert CFA to RGB.
    if ((isDng && photometric == PHOTOMETRIC_CFA) || photometric == PHOTOMETRIC_LINEAR_RAW) {
        co_await linearizeAndNormalizeRawDng(
            tif, dataSampleFormat, dataBitsPerSample, samplesPerPixel, numColorChannels, numRgbaChannels, floatRgbaData, size, priority
        );
    }

    if (shallDemosaic && photometric == PHOTOMETRIC_CFA) {
        if (samplesPerPixel != 1 || numColorChannels != 1 || numRgbaChannels != 1) {
            throw ImageLoadError{"CFA images must have exactly 1 sample per pixel / color / rgba channel."};
        }

        // For CFA images, we need to do demosaicing before we can apply any color space conversion, so we have to write into a staging buffer first.
        HeapArray<float> floatCfaDataBuffer = std::move(floatRgbaDataBuffer);
        span<float> floatCfaData = floatRgbaData;

        numRgbaChannels = numColorChannels = samplesPerPixel = 3;
        numInterleavedChannels = nextSupportedTextureChannelCount(numRgbaChannels);
        auto rgbaChannels = co_await ImageLoader::makeRgbaInterleavedChannels(
            numRgbaChannels, numInterleavedChannels, false, size, EPixelFormat::F32, resultData.channels.front().desiredPixelFormat(), partName, priority
        );

        if (numRgbaChannels == numInterleavedChannels) {
            floatRgbaData = {rgbaChannels.front().floatData(), (size_t)size.x() * size.y() * numRgbaChannels};
        } else {
            floatRgbaDataBuffer.resize((size_t)size.x() * size.y() * numRgbaChannels);
            floatRgbaData = floatRgbaDataBuffer;
        }

        co_await demosaicCfa(tif, floatCfaData, floatRgbaData, size, priority);

        photometric = isDng ? PHOTOMETRIC_LINEAR_RAW : PHOTOMETRIC_RGB;

        resultData.channels.erase(resultData.channels.begin(), resultData.channels.begin() + 1);
        resultData.channels.insert(
            resultData.channels.begin(), make_move_iterator(rgbaChannels.begin()), make_move_iterator(rgbaChannels.end())
        );
    }

    // If no ICC profile is available, we can try to convert the color space manually using TIFF's chromaticity data and transfer function.
    if (compression == COMPRESSION_PIXARLOG) {
        // If we're a Pixar log image, the data is already linear
    } else if (photometric == PHOTOMETRIC_LINEAR_RAW) {
        if (samplesPerPixel < numColorChannels) {
            throw ImageLoadError{"Linear RAW images must have at least as many samples per pixel as color channels."};
        }

        co_await postprocessLinearRawDng(
            tif, samplesPerPixel, numColorChannels, numRgbaChannels, floatRgbaData, resultData, reverseEndian, priority
        );
    } else if (photometric == PHOTOMETRIC_LOGLUV || photometric == PHOTOMETRIC_LOGL) {
        // If we're a LogLUV image, we've already configured the encoder to give us linear XYZ data, so we can just convert that to Rec.709.
        resultData.toRec709 = xyzToChromaMatrix(rec709Chroma());
    } else if (photometric <= PHOTOMETRIC_PALETTE) {
        co_await postprocessRgb(tif, photometric, dataBitsPerSample, numColorChannels, numRgbaChannels, floatRgbaData, resultData, priority);
    } else {
        // Other photometric interpretations do not need a transfer
        resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;
    }

    if (floatRgbaData.data() != resultData.channels.front().floatData()) {
        co_await toFloat32<float, false>(
            (const float*)floatRgbaData.data(), numRgbaChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority
        );
    }

    co_return resultData;
}

Task<vector<ImageData>> TiffImageLoader::load(istream& iStream, const fs::path& path, string_view, int priority, const GainmapHeadroom&) const {
    // This function tries to implement the most relevant parts of the TIFF 6.0 spec:
    // https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf
    // It became quite huge, but there are still many things that are not supported, most notably the photometric specifiers for CIELAB, CIE Lab, and ICC Lab.
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

    TiffData data(buffer.data() + sizeof(Exif::FOURCC), fileSize);
    TIFF* tif = TIFFClientOpen(
        toString(path).c_str(),
        "rMc", // read-only w/ memory mapping; no strip chopping
        reinterpret_cast<thandle_t>(&data),
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

    const auto dngSubFileTypeToString = [&](uint32_t subFileType) {
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
            default: return "unknown";
        }
    };

    // The following code reads all images contained in main-IDFs and sub-IDFs of the TIFF file as per https://libtiff.gitlab.io/libtiff/multi_page.html
    vector<ImageData> result;

    const auto tryLoadImage = [&](tdir_t dir, int subId, int subChainId) -> Task<void> {
        string name;
        if (EDngSubfileType type; isDng && TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &type)) {
            name = dngSubFileTypeToString(type);
        } else {
            name = subId != -1 ? fmt::format("ifd.{}.subifd.{}.{}", dir, subId, subChainId) : fmt::format("ifd.{}", dir);
        }

        try {
            tlog::debug() << fmt::format("Loading '{}'", name);

            ImageData& data = result.emplace_back(co_await readTiffImage(tif, isDng, true, reverseEndian, name, priority));
            if (exifAttributes) {
                data.attributes.emplace_back(exifAttributes.value());
            }

            // Propagate orientation from the main image to sub-images if loading a DNG
            if (isDng && result.size() >= 2) {
                data.orientation = result.at(result.size() - 2).orientation;
            }
        } catch (const ImageLoadError& e) { tlog::warning() << fmt::format("Failed to load '{}': {}", name, e.what()); }
    };

    // The first directory is already read through TIFFOpen()
    do {
        const tdir_t currentDirNumber = TIFFCurrentDirectory(tif);

        co_await tryLoadImage(currentDirNumber, -1, -1);

        // Check if there are SubIFD subfiles
        const toff_t* offsets;
        int numSubIfds = 0;
        if (TIFFGetField(tif, TIFFTAG_SUBIFD, &numSubIfds, &offsets)) {
            // Make a copy of the offsets, as they are only valid until the next TIFFReadDirectory() call
            vector<toff_t> subIfdOffsets(offsets, offsets + numSubIfds);
            for (int i = 0; i < numSubIfds; i++) {
                // Read first SubIFD directory
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
            if (!TIFFSetDirectory(tif, currentDirNumber)) {
                throw ImageLoadError{"Failed to read main IFD."};
            }
        }

        // Read next main-IFD directory (subfile)
    } while (TIFFReadDirectory(tif));

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
