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
#include <tev/imageio/Exif.h>
#include <tev/imageio/TiffImageLoader.h>

#include <half.h>
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
    // Convert lower-bit depth float formats to 32 bit
    if (kind == ETiffKind::F16) {
        size_t numSamples = (size_t)size.x() * size.y() * numSppIn;
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0, numSamples, [&](size_t i) { *(float*)&imageData[i] = *(half*)&imageData[i]; }, priority
        );

        kind = ETiffKind::F32;
    } else if (kind == ETiffKind::F24) {
        size_t numSamples = (size_t)size.x() * size.y() * numSppIn;
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
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

Task<void> postprocessLinearRawDng(
    TIFF* tif,
    const uint16_t dataBitsPerSample,
    const uint16_t samplesPerPixel,
    const int numColorChannels,
    const int numRgbaChannels,
    span<float> floatRgbaData,
    ImageData& resultData,
    const bool reverseEndian,
    const int priority
) {
    // We follow page 96 of https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_0_0.pdf
    tlog::debug() << "Mapping LinearRAW to linear RGB...";

    Vector2i size = resultData.size();
    Box2i activeArea = {Vector2i(0, 0), size};
    if (const TIFFField* field = TIFFFindField(tif, TIFFTAG_ACTIVEAREA, TIFF_ANY)) {
        tlog::debug() << "Found active area data; applying...";
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

        resultData.displayWindow = activeArea;
    }

    tlog::debug() << fmt::format("Active area: min={} max={}", activeArea.min, activeArea.max);

    // Utility var that we'll reuse whenever reading a variable TIFF array
    uint32_t numRead = 0;
    const size_t maxVal = (1ull << dataBitsPerSample) - 1;
    const float scale = 1.0f / maxVal;

    // 1. Map colors via linearization table if it exists
    if (uint16_t* linTable; TIFFGetField(tif, TIFFTAG_LINEARIZATIONTABLE, &numRead, &linTable)) {
        tlog::debug() << "Found linearization table; applying...";

        const size_t maxIdx = numRead - 1;

        co_await ThreadPool::global().parallelForAsync<int>(
            activeArea.min.y(),
            activeArea.max.y(),
            [&](int y) {
                for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                    size_t i = (size_t)y * size.x() + x;
                    for (int c = 0; c < numColorChannels; ++c) {
                        float val = floatRgbaData[i * numRgbaChannels + c];

                        // Lerp the transfer function
                        size_t idx = clamp((size_t)(val * maxVal), (size_t)0, maxIdx - 1);
                        float w = val * maxIdx - idx;
                        floatRgbaData[i * numRgbaChannels + c] = (1.0f - w) * linTable[idx] * scale + w * linTable[idx + 1] * scale;
                    }
                }
            },
            priority
        );
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
                        throw ImageLoadError{"Invalid number of black level pixels."};
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
                        throw ImageLoadError{"Invalid number of black level pixels."};
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
                        throw ImageLoadError{"Invalid number of black level pixels."};
                    }

                    for (size_t i = 0; i < blackLevel.size(); ++i) {
                        blackLevel[i] = blackLevelFloat[i];
                        tlog::debug() << fmt::format("Black rational level[{}] = {}", i, blackLevel[i]);
                    }
                } else {
                    throw ImageLoadError{"Failed to read short black level data."};
                }
                break;
            default: throw ImageLoadError{fmt::format("Unsupported black level data type: {}", (uint32_t)TIFFFieldDataType(field))};
        }

        vector<float> blackLevelDeltaH(activeArea.size().x(), 0.0f);
        vector<float> blackLevelDeltaV(activeArea.size().y(), 0.0f);

        if (float* bldh; TIFFGetField(tif, TIFFTAG_BLACKLEVELDELTAH, &numRead, &bldh)) {
            tlog::debug() << fmt::format("Found {} black level H entries", numRead);
            if (numRead != blackLevelDeltaH.size()) {
                throw ImageLoadError{"Invalid number of black level delta H pixels."};
            }

            for (size_t i = 0; i < blackLevelDeltaH.size(); ++i) {
                blackLevelDeltaH[i] = bldh[i];
            }
        }

        if (float* bldv; TIFFGetField(tif, TIFFTAG_BLACKLEVELDELTAV, &numRead, &bldv)) {
            tlog::debug() << fmt::format("Found {} black level V entries", numRead);
            if (numRead != blackLevelDeltaV.size()) {
                throw ImageLoadError{"Invalid number of black level delta V pixels."};
            }

            for (size_t i = 0; i < blackLevelDeltaV.size(); ++i) {
                blackLevelDeltaV[i] = bldv[i];
            }
        }

        vector<float> maxBlackLevelY(samplesPerPixel * activeArea.size().y(), 0.0f);
        co_await ThreadPool::global().parallelForAsync<int>(
            activeArea.min.y(),
            activeArea.max.y(),
            [&](const int y) {
                int yIdx = y - activeArea.min.y();

                const float deltaV = blackLevelDeltaV[yIdx];
                for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                    const float deltaH = blackLevelDeltaH[x - activeArea.min.x()];
                    const float delta = deltaH + deltaV;

                    const size_t idx = (size_t)y * size.x() + x;
                    const size_t blIdx = (y % blackLevelRepeatRows) * blackLevelRepeatCols + (x % blackLevelRepeatCols);

                    for (int c = 0; c < numColorChannels; ++c) {
                        const float bl = blackLevel[blIdx * samplesPerPixel + c] + delta;
                        floatRgbaData[idx * numRgbaChannels + c] -= bl;

                        maxBlackLevelY[yIdx * samplesPerPixel + c] = std::max(maxBlackLevelY[yIdx * samplesPerPixel + c], bl);
                    }
                }
            },
            priority
        );

        maxBlackLevel.assign(samplesPerPixel, numeric_limits<float>::lowest());
        for (int y = activeArea.min.y(); y < activeArea.max.y(); ++y) {
            int yIdx = y - activeArea.min.y();
            for (int c = 0; c < numColorChannels; ++c) {
                maxBlackLevel[c] = std::max(maxBlackLevel[c], maxBlackLevelY[yIdx * samplesPerPixel + c]);
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
                        throw ImageLoadError{"Invalid number of white level pixels."};
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
                        throw ImageLoadError{"Invalid number of white level pixels."};
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
        tlog::debug() << "Non-1.0 channel scale";

        const size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
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
        const size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    floatRgbaData[i * numRgbaChannels + c] = std::min(floatRgbaData[i * numRgbaChannels + c], 1.0f);
                }
            },
            priority
        );
    }

    // 5. Apply camera to XYZ matrix
    if (samplesPerPixel != 3) {
        throw ImageLoadError{"Linear RAW images with sampledPerPixel != 3 are not supported."};
    }

    // Camera parameters are stored in IFD 0, so let's switch to it temporarily.
    tdir_t prevIdf = TIFFCurrentDirectory(tif);
    TIFFSetDirectory(tif, 0);
    ScopeGuard guard{[&]() { TIFFSetDirectory(tif, prevIdf); }};

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

            tlog::debug() << fmt::format("Found camera calibration matrix: {}", colorMatrix);
        }

        Matrix3f chromaticAdaptation{1.0f};

        // From preliminary tests, it seems that the color matrix from the DNG file is usually already adapted to D50, so we don't
        // need to apply any extra chromatic adaptation. However, the commented out code below shows how to adapt to D50 based on DNG
        // illuminant data if needed.
        const bool adaptToD50 = false;
        if (adaptToD50) {
            if (uint16_t illu; TIFFGetField(tif, CALTAG, &illu)) {
                EExifLightSource illuminant = static_cast<EExifLightSource>(illu);
                tlog::debug() << fmt::format("Found illuminant={}/{}", toString(illuminant), illu);

                const auto whitePoint = xy(illuminant);
                if (whitePoint.x() > 0.0f && whitePoint.y() > 0.0f) {
                    tlog::debug() << fmt::format("Adapting known illuminant with CIE1931 xy={} to D50", whitePoint);
                    chromaticAdaptation = adaptToXYZD50Bradford(whitePoint);
                } else {
                    tlog::warning() << fmt::format("Unknown illuminant");
                }
            }
        }

        const auto xyzToCamera = Matrix3f::scale(analogBalance) * cameraCalibration * colorMatrix;
        return optional<Matrix3f>{chromaticAdaptation * inverse(xyzToCamera)};
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
    co_await ThreadPool::global().parallelForAsync<int>(
        activeArea.min.y(),
        activeArea.max.y(),
        [&](int y) {
            for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                size_t i = (size_t)y * size.x() + x;
                rgbData[i] = toRimm * rgbData[i];
            }
        },
        priority
    );

    // Once we're done, we want to convert from RIMM space to sRGB/Rec709
    resultData.toRec709 = chromaToRec709Matrix(proPhotoChroma());

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
            "Found profile dynamic range: version={}, dynamicRange={}, hintMaxOutputValue={}", pdr->version, pdr->dynamicRange, pdr->hintMaxOutputValue
        );

        // Per DNG 1.7.0.0, page 93, a value of 1 refers to HDR images that need to be compressed into 0-1 before the following transforms
        // take place.
        isHdr = pdr->dynamicRange == 1;
    }

    float exposureScale = 1.0f;
    if (float baselineExposure; TIFFGetField(tif, TIFFTAG_BASELINEEXPOSURE, &baselineExposure)) {
        exposureScale *= exp2f(baselineExposure);
    }

    if (float baselineExposureOffset; TIFFGetField(tif, TIFFTAG_BASELINEEXPOSUREOFFSET, &baselineExposureOffset)) {
        exposureScale *= exp2f(baselineExposureOffset);
    }

    if (exposureScale != 1.0f) {
        tlog::debug() << fmt::format("Applying exposure scale: {}", exposureScale);
        co_await ThreadPool::global().parallelForAsync<int>(
            activeArea.min.y(),
            activeArea.max.y(),
            [&](int y) {
                for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                    size_t i = (size_t)y * size.x() + x;
                    for (int c = 0; c < numColorChannels; ++c) {
                        floatRgbaData[i * numRgbaChannels + c] *= exposureScale;
                    }
                }
            },
            priority
        );
    }

    // At this point, we have the image in linear ProPhoto RGB space. This is most faithful to the readings from the sensor, but the camera
    // may have embedded a (potentially user-chosen) color profile that, per the DNG spec, can be used as a starting point for further user
    // editing. Since tev isn't a photo editor, we don't apply the profile by default, but below is a partial implementation that can apply
    // certain DNG-compatible profiles if needed.
    const bool applyCameraProfile = false;
    if (!applyCameraProfile) {
        co_return;
    }

    {
        // Temporarily switch back to the raw's IFD to read gain table map, if present.
        TIFFSetDirectory(tif, prevIdf);
        ScopeGuard guard2{[&]() { TIFFSetDirectory(tif, 0); }};

        uint32_t numReadGainTableMap = 0;
        if (const uint8_t* gainTableMap; TIFFGetField(tif, TIFFTAG_PROFILEGAINTABLEMAP, &numReadGainTableMap, &gainTableMap)) {
            tlog::warning() << "Found gain table map, but not implemented yet. Color profile may look wrong.";
        }
    }

    if (isHdr) {
        tlog::debug() << "Encoding DNG HDR before applying profile";
        co_await ThreadPool::global().parallelForAsync<int>(
            activeArea.min.y(),
            activeArea.max.y(),
            [&](const int y) {
                for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                    const size_t i = (size_t)y * size.x() + x;
                    for (int c = 0; c < numColorChannels; ++c) {
                        const size_t idx = i * numRgbaChannels + c;
                        floatRgbaData[idx] = dngHdrEncodingFunction(floatRgbaData[idx]);
                    }
                }
            },
            priority
        );
    }

    if (const char* str; TIFFGetField(tif, TIFFTAG_PROFILENAME, &numRead, &str)) {
        tlog::debug() << fmt::format("Applying camera profile \"{}\"", str);
    }

    if (const uint32_t* dims; TIFFGetField(tif, TIFFTAG_PROFILEHUESATMAPDIMS, &dims)) {
        uint32_t hueDivisions = dims[0];
        uint32_t satDivisions = dims[1];
        uint32_t valueDivisions = dims[2];

        tlog::debug() << fmt::format("Hue/sat/val map dimensions: {}x{}x{}", hueDivisions, satDivisions, valueDivisions);

        // TODO: implement hue/sat/val map...
        tlog::warning() << "Found hue/sat/val map, but not implemented yet. Color profile may look wrong.";
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
            size_t i = distance(tc.begin(), it);

            // The spec says to extend the slope of the last segment.
            if (i > 0) {
                --i;
            } else if (i >= tc.size() - 1) {
                i = tc.size() - 2;
            }

            // TODO: Docs say to use cubic spline interpolation, whereas we're using linear interpolation. The difference seems to
            // be negligible so far, but we should fix this at some point.
            const float w = (x - tc[i].x()) / (tc[i + 1].x() - tc[i].x());
            return (1.0f - w) * tc[i].y() + w * tc[i + 1].y();
        };

        co_await ThreadPool::global().parallelForAsync<int>(
            activeArea.min.y(),
            activeArea.max.y(),
            [&](int y) {
                for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                    size_t i = (size_t)y * size.x() + x;
                    for (int c = 0; c < numColorChannels; ++c) {
                        const size_t idx = i * numRgbaChannels + c;
                        floatRgbaData[idx] = applyPwLinear(tc, floatRgbaData[idx]);
                    }
                }
            },
            priority
        );
    }

    if (isHdr) {
        tlog::debug() << "Decoding DNG HDR after applying profile";
        co_await ThreadPool::global().parallelForAsync<int>(
            activeArea.min.y(),
            activeArea.max.y(),
            [&](const int y) {
                for (int x = activeArea.min.x(); x < activeArea.max.x(); ++x) {
                    const size_t i = (size_t)y * size.x() + x;
                    for (int c = 0; c < numColorChannels; ++c) {
                        const size_t idx = i * numRgbaChannels + c;
                        floatRgbaData[idx] = dngHdrDecodingFunction(floatRgbaData[idx]);
                    }
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

    array<Vector2f, 4> chroma = rec709Chroma();
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

    resultData.toRec709 = chromaToRec709Matrix(chroma);

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
    } else if (uint32_t pcsInt; TIFFGetField(tif, TIFFTAG_PREVIEWCOLORSPACE, &pcsInt) && pcsInt > 1) {
        // Alternatively, if we're a preview image from a DNG file, we can use the preview color space to determine the transfer. Values
        // 0 (Unknown) and 1 (Gamma 2.2) are handled by the following `else` block. Other values are handled in this one.
        tlog::debug() << fmt::format("Found preview color space: {}", (uint32_t)pcsInt);

        const EPreviewColorSpace pcs = static_cast<EPreviewColorSpace>(pcsInt);
        // if (pcs == EPreviewColorSpace::AdobeRGB || pcs == EPreviewColorSpace::ProPhotoRGB) {
        //     tlog::warning(
        //     ) << "Linearization from Adobe RGB and ProPhoto RGB is not implemented yet. Using inverse sRGB transfer function instead.";
        // }

        size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                for (int c = 0; c < numColorChannels; ++c) {
                    float v = floatRgbaData[i * numRgbaChannels + c];
                    floatRgbaData[i * numRgbaChannels + c] = toLinear(v);
                }
            },
            priority
        );

        if (pcs == EPreviewColorSpace::AdobeRGB) {
            resultData.toRec709 = chromaToRec709Matrix(adobeChroma());
        } else if (pcs == EPreviewColorSpace::ProPhotoRGB) {
            resultData.toRec709 = chromaToRec709Matrix(proPhotoChroma());
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
    }
}

Task<ImageData> readTiffImage(TIFF* tif, const bool reverseEndian, const int priority) {
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

    if (compression == COMPRESSION_JXL_DNG_1_7) {
        throw ImageLoadError{"DNG JXL compression is unsupported."};
    }

    uint16_t photometric;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric)) {
        throw ImageLoadError{"Failed to read photometric interpretation."};
    }

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

    const uint16_t dataBitsPerSample = bitsPerSample;
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

    tlog::debug() << fmt::format(
        "TIFF info: size={}, bps={}/{}/{}, spp={}, photometric={}, planar={}, sampleFormat={}, compression={}",
        size,
        tiffInternalBitsPerSample,
        dataBitsPerSample,
        bitsPerSample,
        samplesPerPixel,
        photometric,
        planar,
        sampleFormat,
        compression
    );

    // Check if we have an alpha channel
    bool hasAlpha = false;
    bool hasPremultipliedAlpha = false;
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

                hasPremultipliedAlpha = extraChannelTypes[i] == EXTRASAMPLE_ASSOCALPHA;
                hasAlpha = true;
            }
        }
    } else if (samplesPerPixel == 2 || samplesPerPixel == 4) {
        tlog::warning() << "Assuming alpha channel for 2 or 4 samples per pixel.";
        numExtraChannels = 1;
        hasAlpha = true;
    } else {
        numExtraChannels = 0;
    }

    // Determine number of color channels
    int numColorChannels = samplesPerPixel - numExtraChannels;
    int numChannels = samplesPerPixel;

    int numRgbaChannels = numColorChannels + (hasAlpha ? 1 : 0);
    if (numRgbaChannels < 1 || numRgbaChannels > 4) {
        throw ImageLoadError{fmt::format("Unsupported number of RGBA channels: {}", numRgbaChannels)};
    }

    int numNonRgbaChannels = numChannels - numRgbaChannels;
    if (numNonRgbaChannels < 0) {
        throw ImageLoadError{fmt::format("Invalid number of non-RGBA channels: {}", numNonRgbaChannels)};
    }

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

    tlog::debug() << fmt::format("numRgbaChannels={}, numNonRgbaChannels={}, ", numRgbaChannels, numNonRgbaChannels);

    ImageData resultData;
    resultData.dataWindow = resultData.displayWindow = {
        {0, 0},
        size
    };

    uint16_t orientation = 1;
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation)) {
        throw ImageLoadError{"Failed to read orientation."};
    }

    resultData.orientation = static_cast<EOrientation>(orientation);

    // Local scope to prevent use-after-move
    {
        const auto desiredPixelFormat = bitsPerSample > 16 ? EPixelFormat::F32 : EPixelFormat::F16;
        auto rgbaChannels = ImageLoader::makeRgbaInterleavedChannels(numRgbaChannels, hasAlpha, size, EPixelFormat::F32, desiredPixelFormat);
        auto extraChannels = ImageLoader::makeNChannels(numNonRgbaChannels, size, EPixelFormat::F32, desiredPixelFormat);

        resultData.channels.insert(resultData.channels.end(), make_move_iterator(rgbaChannels.begin()), make_move_iterator(rgbaChannels.end()));
        resultData.channels.insert(
            resultData.channels.end(), make_move_iterator(extraChannels.begin()), make_move_iterator(extraChannels.end())
        );
    }

    resultData.hasPremultipliedAlpha = hasPremultipliedAlpha;

    // Read ICC profile if available
    uint32_t iccProfileSize = 0;
    void* iccProfileData = nullptr;

    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccProfileSize, &iccProfileData) && iccProfileSize > 0 && iccProfileData) {
        tlog::debug() << fmt::format("Found ICC color profile of size {} bytes", iccProfileSize);
    }

    // TIFF images are either broken into strips (original format) or tiles (starting with TIFF 6.0). In practice, strips are just tiles
    // with the same width as the image, allowing us to share quite a bit of code below.
    const bool isTiled = TIFFIsTiled(tif);

    struct TileInfo {
        size_t size, rowSize, count, numX, numY;
        uint32_t width, height;
    } tile;
    auto readTile = isTiled ? TIFFReadEncodedTile : TIFFReadEncodedStrip;

    const size_t numPlanes = planar == PLANARCONFIG_CONTIG ? 1 : samplesPerPixel;
    if (isTiled) {
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
        "tile: size={}, count={}, width={}, height={}, numX={}, numY={}", tile.size, tile.count, tile.width, tile.height, tile.numX, tile.numY
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

    vector<uint8_t> tileData(tile.size * tile.count);

    const size_t numTilesPerPlane = tile.count / numPlanes;
    // We'll unpack the bits into 32-bit or 64-bit unsigned integers first, then convert to float. This simplifies the bit unpacking
    const int unpackedBitsPerSample = bitsPerSample > 32 ? 64 : 32;

    const size_t unpackedTileRowSamples = tile.width * samplesPerPixel / numPlanes;
    const size_t unpackedTileSize = tile.height * unpackedTileRowSamples * unpackedBitsPerSample / 8;
    vector<uint8_t> unpackedTile(unpackedTileSize * tile.count);

    const bool handleSign = sampleFormat == SAMPLEFORMAT_INT;

    vector<Task<void>> decodeTasks;

    // Read tiled/striped data. Unfortunately, libtiff doesn't support reading all tiles/strips in parallel, so we have to do that
    // sequentially.
    vector<uint8_t> imageData((size_t)size.x() * size.y() * samplesPerPixel * unpackedBitsPerSample / 8);
    for (size_t i = 0; i < tile.count; ++i) {
        uint8_t* const td = tileData.data() + tile.size * i;

        if (readTile(tif, (uint32_t)i, td, tile.size) < 0) {
            co_await awaitAll(decodeTasks);
            throw ImageLoadError{fmt::format("Failed to read tile {}", i)};
        }

        decodeTasks.emplace_back(
            ThreadPool::global().enqueueCoroutine(
                [&, i, td]() -> Task<void> {
                    uint8_t* const utd = unpackedTile.data() + unpackedTileSize * i;

                    size_t planeTile = i % numTilesPerPlane;
                    size_t tileX = planeTile % tile.numX;
                    size_t tileY = planeTile / tile.numX;

                    int xStart = (int)tileX * tile.width;
                    int xEnd = std::min((int)((tileX + 1) * tile.width), size.x());

                    int yStart = (int)tileY * tile.height;
                    int yEnd = std::min((int)((tileY + 1) * tile.height), size.y());

                    auto unpackTask = [&](auto* const utd, auto* const data) -> Task<void> {
                        co_await ThreadPool::global().parallelForAsync<int>(
                            yStart,
                            yEnd,
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

    float intConversionScale = 1.0f;
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

            intConversionScale = 1.0f;
            break;
        }
        case SAMPLEFORMAT_INT: {
            kind = ETiffKind::I32;
            intConversionScale = 1.0f / ((1ull << (dataBitsPerSample - 1)) - 1);
            break;
        }
        case SAMPLEFORMAT_UINT: {
            if (photometric == PHOTOMETRIC_PALETTE) {
                kind = ETiffKind::Palette;
            } else {
                kind = ETiffKind::U32;
            }

            intConversionScale = 1.0f / ((1ull << dataBitsPerSample) - 1);
            break;
        }
        default: throw ImageLoadError{fmt::format("Unsupported sample format: {}", sampleFormat)};
    }

    bool flipWhiteAndBlack = photometric == PHOTOMETRIC_MINISWHITE;

    // Convert all the extra channels to float and store them in the result data. No further processing needed.
    for (int c = numChannels - numExtraChannels + (hasAlpha ? 1 : 0); c < numChannels; ++c) {
        co_await tiffDataToFloat32<false>(
            kind,
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
    vector<float> floatRgbaData(size.x() * (size_t)size.y() * numRgbaChannels);
    co_await tiffDataToFloat32<false>(
        kind,
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

    // Try color space conversion using ICC profile if available. This is going to be the most accurate method.
    if (iccProfileData && iccProfileSize > 0) {
        try {
            const auto cicp = co_await toLinearSrgbPremul(
                ColorProfile::fromIcc((uint8_t*)iccProfileData, iccProfileSize),
                size,
                numColorChannels,
                hasAlpha ? (hasPremultipliedAlpha ? EAlphaKind::Premultiplied : EAlphaKind::Straight) : EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)floatRgbaData.data(),
                resultData.channels.front().floatData(),
                4,
                priority
            );

            if (cicp) {
                resultData.hdrMetadata.whiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicp->transfer);
            }

            resultData.hasPremultipliedAlpha = true;
            co_return resultData;
        } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    // If no ICC profile is available, we can try to convert the color space manually using TIFF's chromaticity data and transfer function.
    if (compression == COMPRESSION_PIXARLOG) {
        // If we're a Pixar log image, the data is already linear
    } else if (photometric == PHOTOMETRIC_LINEAR_RAW) {
        if (samplesPerPixel < numColorChannels) {
            throw ImageLoadError{"Linear RAW images must have at least as many samples per pixel as color channels."};
        }

        co_await postprocessLinearRawDng(
            tif, dataBitsPerSample, samplesPerPixel, numColorChannels, numRgbaChannels, floatRgbaData, resultData, reverseEndian, priority
        );
    } else if (photometric == PHOTOMETRIC_LOGLUV || photometric == PHOTOMETRIC_LOGL) {
        // If we're a LogLUV image, we've already configured the encoder to give us linear XYZ data, so we can just convert that to Rec.709.
        resultData.toRec709 = xyzToRec709Matrix();
    } else if (photometric <= PHOTOMETRIC_PALETTE) {
        co_await postprocessRgb(tif, photometric, dataBitsPerSample, numColorChannels, numRgbaChannels, floatRgbaData, resultData, priority);
    } else {
        // Other photometric interpretations do not need a transfer
    }

    co_await toFloat32<float, false>(
        (const float*)floatRgbaData.data(), numRgbaChannels, resultData.channels.front().floatData(), 4, size, hasAlpha, priority
    );

    co_return resultData;
}

Task<vector<ImageData>> TiffImageLoader::load(istream& iStream, const fs::path& path, string_view, int priority, bool applyGainmaps) const {
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

    // Read the entire stream into memory and decompress from there. Technically, we can progressively decode TIFF images, but we want to
    // additionally load the TIFF image via our EXIF library, which requires the file to be in memory. For the same reason, we also prepend
    // the EXIF FOURCC to the data ahead of the TIFF header.
    iStream.seekg(0, ios::end);
    const size_t fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    vector<uint8_t> buffer(fileSize + sizeof(Exif::FOURCC));
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

    enum EDngSubfileType : uint16_t {
        Main = 0,
        Reduced = 1,
        Transparency = 4,
        TransparencyReduced = 5,
        Depth = 8,
        DepthReduced = 9,
        Enhanced = 16,
    };

    auto dngSubFileTypeToString = [&](uint32_t subFileType) {
        switch (subFileType) {
            case Main: return "main";
            case Reduced: return "reduced";
            case Transparency: return "main.transparency";
            case TransparencyReduced: return "reduced.transparency";
            case Depth: return "main.depth";
            case DepthReduced: return "reduced.depth";
            case Enhanced: return "main.enhanced";
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
            name = subId != -1 ? fmt::format("main.{}.sub.{}.{}", dir, subId, subChainId) : fmt::format("main.{}", dir);
        }

        try {
            tlog::debug() << fmt::format("Loading {}", name);

            ImageData& data = result.emplace_back(co_await readTiffImage(tif, reverseEndian, priority));
            data.partName = name;

            if (exifAttributes) {
                data.attributes.emplace_back(exifAttributes.value());
            }

            // Propagate orientation from the main image to sub-images if loading a DNG
            if (isDng && result.size() >= 2) {
                data.orientation = result.at(result.size() - 2).orientation;
            }
        } catch (const ImageLoadError& e) { tlog::warning() << fmt::format("Failed to load {}: {}", name, e.what()); }
    };

    // The first directory is already read through TIFFOpen()
    do {
        tdir_t currentDirNumber = TIFFCurrentDirectory(tif);

        co_await tryLoadImage(currentDirNumber, -1, -1);

        // Check if there are SubIFD subfiles
        toff_t* offsets;
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

    // No need to label the parts if it turns out there's just one image in this TIFF container.
    if (result.size() == 1) {
        result.front().partName = "";
    }

    co_return result;
}

} // namespace tev
