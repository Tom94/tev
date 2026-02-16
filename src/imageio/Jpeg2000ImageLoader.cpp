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
#include <tev/imageio/Jpeg2000ImageLoader.h>

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

Task<vector<ImageData>> Jpeg2000ImageLoader::load(
    span<const uint8_t> data,
    const fs::path& path,
    string_view channelSelector,
    int priority,
    const GainmapHeadroom& gainmapHeadroom,
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

    resultData.dataWindow = resultData.displayWindow = region;

    const bool hasAlpha = numChannels == 2 || numChannels >= 4;
    const auto numInterleavedChannels = numChannels == 1 ? 1 : 4;
    const auto numColorChannels = hasAlpha ? std::min(numChannels, (size_t)4) - 1 : std::min(numChannels, (size_t)4);
    const auto numExtraChannels = numChannels > 4 ? numChannels - 4 : 0;

    if (numInterleavedChannels == 1) {
        resultData.channels.emplace_back(Channel::joinIfNonempty(resultData.partName, "L"), size, EPixelFormat::F32, EPixelFormat::F16);
    } else {
        resultData.channels = co_await makeRgbaInterleavedChannels(
            numColorChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, resultData.partName, priority
        );
    }

    for (size_t c = 0; c < numExtraChannels; ++c) {
        resultData.channels.emplace_back(fmt::format("extra.{}", c), size, EPixelFormat::F32, EPixelFormat::F16);
    }

    // If there is an alpha channel, it's usually straight. TODO: read cdef box if present to be sure.
    resultData.hasPremultipliedAlpha = !hasAlpha;

    const auto numPixels = (size_t)size.x() * size.y();

    const auto toFloat = [&](float* rgba, bool convertSrgbToLinear) -> Task<void> {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels * numChannels,
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    Vector4f color = {0.0f, 0.0f, 0.0f, 1.0f};
                    for (size_t c = 0; c < numChannels; ++c) {
                        const auto& comp = image->comps[c];
                        const auto xc = ((x - (int)comp.x0 + (int)image->x0) / (int)comp.dx) >> comp.factor;
                        const auto yc = ((y - (int)comp.y0 + (int)image->y0) / (int)comp.dy) >> comp.factor;

                        float v = 0.0f;
                        if (yc >= 0 && yc < (int)image->comps[c].h && xc >= 0 && xc < (int)image->comps[c].w) {
                            v = (float)image->comps[c].data[yc * image->comps[c].w + xc] /
                                ((1ull << (image->comps[c].prec - image->comps[c].sgnd)) - 1);
                        }

                        if (c < 4) {
                            const auto targetChannel = hasAlpha && c == numColorChannels ? 3 : c;
                            color[targetChannel] = v;
                        } else {
                            resultData.channels[c].setAt({x, y}, v);
                        }
                    }

                    if (colorSpace == OPJ_CLRSPC_SYCC || colorSpace == OPJ_CLRSPC_EYCC) {
                        Vector3f rgb = yccToRgb(color.x(), color.y(), color.z());
                        color.x() = rgb.x();
                        color.y() = rgb.y();
                        color.z() = rgb.z();
                    }

                    if (convertSrgbToLinear) {
                        for (size_t c = 0; c < numColorChannels; ++c) {
                            color[c] = toLinear(color[c]);
                        }
                    }

                    if (numColorChannels == 1) {
                        color.y() = color.z() = color.x();
                    }

                    for (size_t c = 0; c < numInterleavedChannels; ++c) {
                        rgba[((size_t)y * size.x() + x) * numInterleavedChannels + c] = color[c];
                    }
                }
            },
            priority
        );
    };

    if (!skipColorProcessing && image->icc_profile_buf && image->icc_profile_len > 0) {
        try {
            const auto profile = ColorProfile::fromIcc({image->icc_profile_buf, image->icc_profile_len});

            HeapArray<float> iccTmpFloatData(numPixels * numInterleavedChannels);
            co_await toFloat(iccTmpFloatData.data(), false);

            co_await toLinearSrgbPremul(
                profile,
                size,
                numInterleavedChannels == 1 ? 1 : 3,
                numInterleavedChannels == 1 ? EAlphaKind::None :
                                              (resultData.hasPremultipliedAlpha ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight),
                EPixelFormat::F32,
                (uint8_t*)iccTmpFloatData.data(),
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

    co_await toFloat(resultData.channels.front().floatData(), !skipColorProcessing);

    // TODO: figure out how to extract exif and xmp metadata

    co_return result;
}

Task<vector<ImageData>> Jpeg2000ImageLoader::load(
    istream& iStream, const fs::path& path, string_view channelSelector, int priority, const GainmapHeadroom& gainmapHeadroom
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

    co_return co_await load(data, path, channelSelector, priority, gainmapHeadroom, false);
}

} // namespace tev
