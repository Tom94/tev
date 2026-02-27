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

#include "tev/imageio/ImageLoader.h"
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/Jpeg2000ImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <openjpeg.h>

using namespace nanogui;
using namespace std;

namespace tev {

optional<OPJ_CODEC_FORMAT> detectJ2kFormat(span<const uint8_t> hdr) {
    if (hdr.size() < 4) {
        return nullopt;
    }

    // Raw J2K codestream: SOC marker FF 4F
    if (hdr[0] == 0xFF && hdr[1] == 0x4F) {
        return OPJ_CODEC_J2K;
    }

    // JP2/JPX/JPM/MJ2 box-based format:
    // 00 00 00 0C 6A 50 20 20 0D 0A 87 0A
    static const uint8_t jp2Magic[12] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};

    if (hdr.size() < sizeof(jp2Magic)) {
        return nullopt;
    }

    if (memcmp(hdr.data(), jp2Magic, sizeof(jp2Magic)) == 0) {
        // We could differentiate between JP2 and other codecs, but openjpeg only supports JP2 anyway, so we won't.
        return OPJ_CODEC_JP2;
    }

    return nullopt;
}

struct MemStream {
    span<const uint8_t> data;
    size_t pos;
};

static OPJ_SIZE_T memRead(void* buf, OPJ_SIZE_T n, void* ud) {
    auto* m = static_cast<MemStream*>(ud);
    if (m->pos >= m->data.size()) {
        return (OPJ_SIZE_T)-1;
    }

    OPJ_SIZE_T r = std::min(n, (OPJ_SIZE_T)(m->data.size() - m->pos));
    memcpy(buf, m->data.data() + m->pos, r);
    m->pos += r;
    return r;
}

static OPJ_OFF_T memSkip(OPJ_OFF_T n, void* ud) {
    auto* m = static_cast<MemStream*>(ud);
    if (n < 0) {
        return -1;
    }

    m->pos = std::min(m->pos + (size_t)n, m->data.size());
    return (OPJ_OFF_T)m->pos;
}

static OPJ_BOOL memSeek(OPJ_OFF_T n, void* ud) {
    auto* m = static_cast<MemStream*>(ud);
    if (n < 0 || (size_t)n > m->data.size()) {
        return OPJ_FALSE;
    }

    m->pos = (size_t)n;
    return OPJ_TRUE;
}

static opj_stream_t* makeMemStream(MemStream* m) {
    opj_stream_t* s = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE);
    if (!s) {
        return nullptr;
    }

    opj_stream_set_user_data(s, m, nullptr);
    opj_stream_set_user_data_length(s, m->data.size());
    opj_stream_set_read_function(s, memRead);
    opj_stream_set_skip_function(s, memSkip);
    opj_stream_set_seek_function(s, memSeek);
    return s;
}

// EXIF UUID used in JP2 UUID boxes
inline constexpr array<uint8_t[16], 2> exifUuids = {
    {
     // JpgTiffExif->JP2 (standard, used by ExifTool etc.)
        {0x4A, 0x70, 0x67, 0x54, 0x69, 0x66, 0x66, 0x45, 0x78, 0x69, 0x66, 0x2D, 0x3E, 0x4A, 0x50, 0x32},
     // Adobe Photoshop JPEG2000 plugin v1.5
        {0x05, 0x37, 0xCD, 0xAB, 0x9D, 0x0C, 0x44, 0x31, 0xA7, 0x2A, 0xFA, 0x56, 0x1F, 0x2A, 0x11, 0x3E},
     }
};

// XMP UUID used in JP2 UUID boxes
// BE7ACFCB-97A9-42E8-9C71-999491E3AFAC
static const uint8_t xmpUuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

struct Jp2Box {
    string_view type;
    span<const uint8_t> data;
};

static uint32_t readU32Be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint64_t readU64Be(const uint8_t* p) { return (uint64_t(readU32Be(p)) << 32) | uint64_t(readU32Be(p + 4)); }

struct Jp2Metadata {
    span<const uint8_t> genericXml;
    span<const uint8_t> xmpXml;
    span<const uint8_t> exifData;
};

static optional<Jp2Box> readBoxHeader(span<const uint8_t> data, uint64_t* length) {
    TEV_ASSERT(length, "Length output parameter must not be null.");

    if (data.size() < 8) {
        tlog::warning() << "Invalid JP2 box: insufficient data for 32-bit length.";
        *length = data.size();
        return nullopt;
    }

    span<const uint8_t> boxData;

    const auto len32 = std::min((size_t)readU32Be(data.data()), data.size());
    if (len32 == 1) {
        if (data.size() < 16) {
            tlog::warning() << "Invalid JP2 box: insufficient data for 64-bit length.";
            *length = data.size();
            return nullopt;
        }

        const auto len64 = std::min((size_t)readU64Be(data.data() + 8), data.size());
        boxData = data.subspan(16, len64 - 16);

        *length = len64;
    } else if (len32 == 0) {
        boxData = data.subspan(8);
        *length = data.size();
    } else if (len32 >= 8) {
        boxData = data.subspan(8, len32 - 8);
        *length = len32;
    } else {
        tlog::warning() << fmt::format("Invalid JP2 box: length {} is too small.", len32);
        *length = data.size();
        return nullopt;
    }

    return Jp2Box{
        string_view{(const char*)data.data() + 4, 4},
        boxData
    };
}

Jp2Metadata extractJp2Metadata(span<const uint8_t> data) {
    Jp2Metadata meta;

    tlog::debug() << "Extracting JP2 boxes:";

    while (data.size() > 0) {
        uint64_t boxLength = 0;
        const auto box = readBoxHeader(data, &boxLength);
        if (!box.has_value()) {
            break;
        }

        data = data.subspan(boxLength);
        tlog::debug() << fmt::format("  type='{}' length={}", box->type, boxLength);

        if (box->type == "xml ") {
            meta.genericXml = box->data;
        } else if (box->type == "uuid") {
            if (box->data.size() < 16) {
                tlog::warning() << "Invalid JP2 UUID box: insufficient data for UUID.";
                continue;
            }

            if (memcmp(box->data.data(), xmpUuid, 16) == 0) {
                meta.xmpXml = box->data.subspan(16);
            } else if (any_of(begin(exifUuids), end(exifUuids), [&box](const uint8_t (&knownUuid)[16]) {
                           return memcmp(box->data.data(), knownUuid, 16) == 0;
                       })) {
                meta.exifData = box->data.subspan(16);
            }
        }
    }

    return meta;
}

Task<vector<ImageData>> Jpeg2000ImageLoader::load(
    span<const uint8_t> data,
    const fs::path& path,
    string_view channelSelector,
    const ImageLoaderSettings& settings,
    int priority,
    bool skipColorProcessing,
    size_t* bitsPerSampleOut,
    EPixelType* pixelTypeOut
) const {
    const auto j2kFormat = detectJ2kFormat(data);

    if (!j2kFormat.has_value()) {
        throw FormatNotSupported{"Data is not a JPEG 2000 image or codestream."};
    }

    opj_codec_t* codec = opj_create_decompress(*j2kFormat);
    if (!codec) {
        throw ImageLoadError{"Failed to create JPEG 2000 codec."};
    }

    const ScopeGuard codecGuard{[&] { opj_destroy_codec(codec); }};

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    opj_setup_decoder(codec, &params);

    MemStream ms{data, 0};
    opj_stream_t* stream = makeMemStream(&ms);
    if (!stream) {
        throw ImageLoadError{"Failed to create JPEG 2000 stream."};
    }

    const ScopeGuard streamGuard{[&] { opj_stream_destroy(stream); }};

    opj_image_t* image = nullptr;
    const ScopeGuard imageGuard{[&] {
        if (image) {
            opj_image_destroy(image);
        }
    }};

    if (!opj_read_header(stream, codec, &image) || !image) {
        throw ImageLoadError{"Failed to read JPEG 2000 header."};
    }

    if (!opj_decode(codec, stream, image)) {
        throw ImageLoadError{"Failed to decode JPEG 2000 image."};
    }

    if (!opj_end_decompress(codec, stream)) {
        throw ImageLoadError{"Failed to finalize JPEG 2000 decompression."};
    }

    const auto colorSpaceToString = [](OPJ_COLOR_SPACE cs) -> string_view {
        switch (cs) {
            case OPJ_CLRSPC_UNKNOWN: return "unknown";
            case OPJ_CLRSPC_UNSPECIFIED: return "unspecified";
            case OPJ_CLRSPC_SRGB: return "srgb";
            case OPJ_CLRSPC_GRAY: return "gray";
            case OPJ_CLRSPC_SYCC: return "sycc";
            case OPJ_CLRSPC_EYCC: return "eycc";
            case OPJ_CLRSPC_CMYK: return "cmyk";
            default: return "invalid";
        }
    };

    const auto region = Box2i{
        Vector2i{(int)image->x0,               (int)image->y0              },
        Vector2i{(int)(image->x0 + image->x1), (int)(image->y0 + image->y1)}
    };
    const auto size = region.size();

    tlog::debug() << fmt::format(
        "JPEG 2000 info: region=[{}, {}] numcomps={} color_space={} icc={}",
        region.min,
        region.max,
        image->numcomps,
        colorSpaceToString(image->color_space),
        image->icc_profile_len > 0 ? "yes" : "no"
    );

    const size_t numChannels = image->numcomps;

    for (size_t c = 0; c < image->numcomps; ++c) {
        const auto& comp = image->comps[c];

        tlog::debug() << fmt::format(
            "  Component {}: w={} h={} dx={} dy={} x0={} y0={} prec={} sgnd={} resno_decoded={} factor={} alpha={}",
            c,
            comp.w,
            comp.h,
            comp.dx,
            comp.dy,
            comp.x0,
            comp.y0,
            comp.prec,
            comp.sgnd,
            comp.resno_decoded,
            comp.factor,
            comp.alpha
        );

        if (comp.alpha) {
            if (c != image->numcomps - 1) {
                tlog::warning() << fmt::format("Alpha channel is not the last component (index {}). This is unusual and may cause issues.", c);
            }
        }
    }

    OPJ_COLOR_SPACE colorSpace = image->color_space;
    if (colorSpace == OPJ_CLRSPC_UNSPECIFIED || colorSpace == OPJ_CLRSPC_UNKNOWN) {
        if (numChannels <= 2) {
            colorSpace = OPJ_CLRSPC_GRAY;
        } else {
            colorSpace = OPJ_CLRSPC_SRGB;
        }
    }

    const auto yccToRgb = [](float y, float cb, float cr) {
        cb -= 0.5f;
        cr -= 0.5f;
        return Vector3f{y + (1.402f * cr), y - (0.344136f * cb + 0.714136f * cr), y + (1.772f * cb)};
    };

    vector<ImageData> result(1);
    auto& resultData = result.front();

    // Only a box-based jpeg 2000 image can contain metadata.
    const auto meta = j2kFormat == OPJ_CODEC_JP2 || j2kFormat == OPJ_CODEC_JPX ? extractJp2Metadata(data) : Jp2Metadata{};

    if (!meta.exifData.empty()) {
        tlog::debug() << fmt::format("Found EXIF data of size {} bytes", meta.exifData.size());

        try {
            const auto exif = Exif{meta.exifData};
            resultData.attributes.emplace_back(exif.toAttributes());

            const EOrientation exifOrientation = exif.getOrientation();
            if (exifOrientation != EOrientation::None) {
                resultData.orientation = exifOrientation;
                tlog::debug() << fmt::format("EXIF image orientation: {}", toString(resultData.orientation));
            }
        } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
    }

    if (!meta.xmpXml.empty() || !meta.genericXml.empty()) {
        // Prefer a known-xmp box over a generic xml box, but try the generic one if no xmp box is present just in case it contains xmp.
        const auto xmlData = meta.xmpXml.empty() ? meta.genericXml : meta.xmpXml;
        if (xmlData.data() == meta.genericXml.data()) {
            tlog::debug() << fmt::format(
                "Found generic XML metadata of size {} bytes. No XMP-specific box found, trying to parse as XMP anyway.", xmlData.size()
            );
        } else {
            tlog::debug() << fmt::format("Found XMP metadata of size {} bytes.", xmlData.size());
        }

        const string_view xmpDataView = string_view{(const char*)xmlData.data(), xmlData.size()};

        try {
            const auto xmp = Xmp{xmpDataView};
            resultData.attributes.emplace_back(xmp.attributes());

            const EOrientation xmpOrientation = xmp.orientation();
            if (xmpOrientation != EOrientation::None) {
                resultData.orientation = xmpOrientation;
                tlog::debug() << fmt::format("XMP image orientation: {}", toString(resultData.orientation));
            }
        } catch (const invalid_argument& e) {
            if (xmlData.data() == meta.genericXml.data()) {
                tlog::debug() << fmt::format("Failed to parse XML data as XMP: {}", e.what());
            } else {
                tlog::warning() << fmt::format("Failed to parse XMP metadata: {}", e.what());
            }
        }
    }

    resultData.dataWindow = resultData.displayWindow = region;

    const bool hasAlpha = numChannels == 2 || numChannels >= 4;
    const auto numRgbaChannels = std::min(numChannels, (size_t)4);
    const auto numInterleavedChannels = nextSupportedTextureChannelCount(numRgbaChannels);
    const auto numColorChannels = hasAlpha ? numRgbaChannels - 1 : numRgbaChannels;
    const auto numExtraChannels = numChannels > numRgbaChannels ? numChannels - numRgbaChannels : 0;

    resultData.channels = co_await makeRgbaInterleavedChannels(
        numRgbaChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, resultData.partName, priority
    );

    for (size_t c = 0; c < numExtraChannels; ++c) {
        resultData.channels.emplace_back(fmt::format("extra.{}", c), size, EPixelFormat::F32, EPixelFormat::F16);
    }

    // If there is an alpha channel, it's usually straight. TODO: read cdef box if present to be sure.
    resultData.hasPremultipliedAlpha = !hasAlpha;

    const auto numPixels = (size_t)size.x() * size.y();

    const auto getChannelValue = [&image](size_t c, int x, int y) -> float {
        const auto& comp = image->comps[c];
        const auto xc = ((x - (int)comp.x0 + (int)image->x0) / (int)comp.dx) >> comp.factor;
        const auto yc = ((y - (int)comp.y0 + (int)image->y0) / (int)comp.dy) >> comp.factor;

        if (yc >= 0 && yc < (int)comp.h && xc >= 0 && xc < (int)comp.w) {
            return (float)comp.data[yc * comp.w + xc] / ((1ull << (comp.prec - comp.sgnd)) - 1);
        } else {
            return 0.0f;
        }
    };

    // First copy over the extra channels -- they are treated the same way, regardless of color space settings
    if (numExtraChannels > 0) {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels * numExtraChannels,
            [&](int y) {
                for (size_t c = numRgbaChannels; c < numChannels; ++c) {
                    for (int x = 0; x < size.x(); ++x) {
                        resultData.channels[c].dynamicSetAt({x, y}, getChannelValue(c, x, y));
                    }
                }
            },
            priority
        );
    }

    // Then we handle RGBA channels together, depending on color space and presence of ICC profile
    const auto rgbaToFloat = [&](float* rgba, size_t outNumChannels, bool convertSrgbToLinear) -> Task<void> {
        TEV_ASSERT(numColorChannels > 0 && numColorChannels <= 3, "Invalid number of color channels.");
        TEV_ASSERT(outNumChannels >= numRgbaChannels, "Output buffer must have enough channels for RGBA data.");
        TEV_ASSERT(outNumChannels <= 4, "Output buffer cannot have more than 4 channels.");

        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels * numRgbaChannels,
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    Vector3f rgb{0.0f};
                    for (size_t c = 0; c < numColorChannels; ++c) {
                        rgb[c] = getChannelValue(c, x, y);
                    }

                    if (colorSpace == OPJ_CLRSPC_SYCC || colorSpace == OPJ_CLRSPC_EYCC) {
                        rgb = yccToRgb(rgb.x(), rgb.y(), rgb.z());
                    }

                    if (convertSrgbToLinear) {
                        for (size_t c = 0; c < numColorChannels; ++c) {
                            rgb[c] = toLinear(rgb[c]);
                        }
                    }

                    for (size_t c = 0; c < numColorChannels; ++c) {
                        rgba[((size_t)y * size.x() + x) * outNumChannels + c] = rgb[std::min(c, numColorChannels - 1)];
                    }

                    if (hasAlpha) {
                        rgba[((size_t)y * size.x() + x) * outNumChannels + outNumChannels - 1] = getChannelValue(numColorChannels, x, y);
                    }
                }
            },
            priority
        );
    };

    if (!skipColorProcessing && image->icc_profile_buf && image->icc_profile_len > 0) {
        try {
            const auto profile = ColorProfile::fromIcc({image->icc_profile_buf, image->icc_profile_len});

            HeapArray<float> iccTmpFloatData(numPixels * numRgbaChannels);
            co_await rgbaToFloat(iccTmpFloatData.data(), numRgbaChannels, false);

            co_await toLinearSrgbPremul(
                profile,
                size,
                numColorChannels,
                hasAlpha ? (resultData.hasPremultipliedAlpha ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight) : EAlphaKind::None,
                iccTmpFloatData.data(),
                resultData.channels.front().floatData(),
                numInterleavedChannels,
                nullopt,
                priority
            );
            resultData.hasPremultipliedAlpha = true;
            resultData.readMetadataFromIcc(profile);

            co_return result;
        } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    co_await rgbaToFloat(resultData.channels.front().floatData(), numInterleavedChannels, !skipColorProcessing);

    resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
    resultData.nativeMetadata.chroma = rec709Chroma();

    co_return result;
}

Task<vector<ImageData>> Jpeg2000ImageLoader::load(
    istream& iStream, const fs::path& path, string_view channelSelector, const ImageLoaderSettings& settings, int priority
) const {
    const size_t initialPos = iStream.tellg();

    uint8_t magic[12];
    iStream.read((char*)magic, 12);

    if (!detectJ2kFormat({magic, (size_t)iStream.tellg() - initialPos}).has_value()) {
        throw FormatNotSupported{"File is not a JPEG 2000 image or codestream."};
    }

    iStream.clear();
    iStream.seekg(0, iStream.end);
    const auto dataSize = (size_t)iStream.tellg() - initialPos;
    iStream.seekg(initialPos, iStream.beg);

    HeapArray<uint8_t> data(dataSize);
    iStream.read((char*)data.data(), dataSize);

    co_return co_await load(data, path, channelSelector, settings, priority, false);
}

} // namespace tev
