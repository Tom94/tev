/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
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

#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/JxrImageLoader.h>

#ifndef FAR
#    define FAR
#endif

#include <JXRGlue.h>
#include <JXRMeta.h>

#ifdef min
#    undef min
#endif

#ifdef max
#    undef max
#endif

using namespace nanogui;
using namespace std;

namespace tev {

// jxrlib reports failures via an HRESULT-like ERR code; this turns a non-zero code into a thrown error.
#define JXR_CHECK(call)                                                                                   \
    do {                                                                                                  \
        ERR jxrErr_ = (call);                                                                             \
        if (Failed(jxrErr_)) {                                                                            \
            throw ImageLoadError{fmt::format("JXR error {} at {}:{}", (int)jxrErr_, __FILE__, __LINE__)}; \
        }                                                                                                 \
    } while (0)

namespace {

enum class EJxrColorModel {
    RGB, // includes gray and RGBA
    CMYK,
    YCC,
    NChannel,
};

string_view toString(EJxrColorModel model) {
    switch (model) {
        case EJxrColorModel::RGB: return "rgb";
        case EJxrColorModel::CMYK: return "cmyk";
        case EJxrColorModel::YCC: return "ycc";
        case EJxrColorModel::NChannel: return "n-channel";
    }
    return "unknown";
}

struct JxrFormat {
    size_t numColorChannels = 0; // color channels in the RGB(A) view tev consumes (1 for gray, 3 for color)
    size_t numChannels = 0;      // total incl. alpha in that view
    EPixelFormat pixelFormat = EPixelFormat::U8; // component type as decoded RAW (pre-conversion)
    bool isFloat = false;        // half or float: linear, scene-referred
    bool isFixedPoint = false;   // fixed point: linear, scene-referred, integer-stored
    bool hasAlpha = false;       // format actually carries an alpha plane
    bool bgrOrder = false;       // stored BGR(A); needs channel swap if consumed raw
    bool rawConsumable = false;  // tev can read this layout directly without the converter
    EJxrColorModel colorModel = EJxrColorModel::RGB;
    optional<PKPixelFormatGUID> target = nullopt;
    bool hasPremultipliedAlpha = false;
};

optional<JxrFormat> describeFormat(const PKPixelFormatGUID& guid) {
    auto eq = [&](const PKPixelFormatGUID& other) { return IsEqualGUID(guid, other); };

    using F = JxrFormat;
    using PF = EPixelFormat;
    using CM = EJxrColorModel;

    if (eq(GUID_PKPixelFormatDontCare)) {
        return nullopt; // not a concrete format
    }

    // Gray (RGB color model, 1 color channel)
    else if (eq(GUID_PKPixelFormatBlackWhite)) {
        return F{1, 1, PF::U8, false, false, false, false, true, CM::RGB, GUID_PKPixelFormat8bppGray};
    } else if (eq(GUID_PKPixelFormat8bppGray)) {
        return F{1, 1, PF::U8, false, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat16bppGray)) {
        return F{1, 1, PF::U16, false, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat16bppGrayHalf)) {
        return F{1, 1, PF::F16, true, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppGrayFloat)) {
        return F{1, 1, PF::F32, true, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat16bppGrayFixedPoint)) {
        return F{1, 1, PF::I16, false, true, false, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppGrayFixedPoint)) {
        return F{1, 1, PF::I32, false, true, false, false, false, CM::RGB};
    }

    // Packed RGB needing unpacking
    else if (eq(GUID_PKPixelFormat16bppRGB555)) {
        return F{3, 3, PF::U8, false, false, false, false, false, CM::RGB, GUID_PKPixelFormat24bppRGB};
    } else if (eq(GUID_PKPixelFormat16bppRGB565)) {
        return F{3, 3, PF::U8, false, false, false, false, false, CM::RGB, GUID_PKPixelFormat24bppRGB};
    } else if (eq(GUID_PKPixelFormat32bppRGB101010)) {
        return F{3, 3, PF::U16, false, false, false, false, false, CM::RGB, GUID_PKPixelFormat48bppRGB};
    } else if (eq(GUID_PKPixelFormat32bppRGBE)) {
        return F{3, 3, PF::F32, true, false, false, false, false, CM::RGB, GUID_PKPixelFormat96bppRGBFloat};
    }

    // 8-bit RGB(A)
    else if (eq(GUID_PKPixelFormat24bppRGB)) {
        return F{3, 3, PF::U8, false, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat24bppBGR)) {
        return F{3, 3, PF::U8, false, false, false, true, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppRGB)) {
        return F{3, 4, PF::U8, false, false, false, false, true, CM::RGB}; // 4th channel is padding
    } else if (eq(GUID_PKPixelFormat32bppBGR)) {
        return F{3, 4, PF::U8, false, false, false, true, true, CM::RGB}; // 4th channel is padding
    } else if (eq(GUID_PKPixelFormat32bppRGBA)) {
        return F{3, 4, PF::U8, false, false, true, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppBGRA)) {
        return F{3, 4, PF::U8, false, false, true, true, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppPRGBA)) {
        return F{3, 4, PF::U8, false, false, true, false, true, CM::RGB, GUID_PKPixelFormat32bppPRGBA, true}; // premultiplied
    } else if (eq(GUID_PKPixelFormat32bppPBGRA)) {
        return F{3, 4, PF::U8, false, false, true, true, true, CM::RGB, GUID_PKPixelFormat32bppPBGRA, true}; // premultiplied
    }

    // 16-bit RGB(A)
    else if (eq(GUID_PKPixelFormat48bppRGB)) {
        return F{3, 3, PF::U16, false, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBA)) {
        return F{3, 4, PF::U16, false, false, true, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppPRGBA)) {
        return F{3, 4, PF::U16, false, false, true, false, true, CM::RGB, GUID_PKPixelFormat64bppPRGBA, true}; // premultiplied
    }

    // Fixed point RGB(A)
    else if (eq(GUID_PKPixelFormat48bppRGBFixedPoint)) {
        return F{3, 3, PF::I16, false, true, false, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBFixedPoint)) {
        return F{3, 4, PF::I16, false, true, false, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBAFixedPoint)) {
        return F{3, 4, PF::I16, false, true, true, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat96bppRGBFixedPoint)) {
        return F{3, 3, PF::I32, false, true, false, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBFixedPoint)) {
        return F{3, 4, PF::I32, false, true, false, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBAFixedPoint)) {
        return F{3, 4, PF::I32, false, true, true, false, false, CM::RGB};
    }

    // Half float RGB(A)
    else if (eq(GUID_PKPixelFormat48bppRGBHalf)) {
        return F{3, 3, PF::F16, true, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBHalf)) {
        return F{3, 4, PF::F16, true, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBAHalf)) {
        return F{3, 4, PF::F16, true, false, true, false, true, CM::RGB};
    }

    // 32-bit float RGB(A)
    else if (eq(GUID_PKPixelFormat96bppRGBFloat)) {
        return F{3, 3, PF::F32, true, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBFloat)) {
        return F{3, 4, PF::F32, true, false, false, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBAFloat)) {
        return F{3, 4, PF::F32, true, false, true, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppPRGBAFloat)) {
        return F{3, 4, PF::F32, true, false, true, false, true, CM::RGB, GUID_PKPixelFormat128bppPRGBAFloat, true}; // premultiplied
    }

    // CMYK
    else if (eq(GUID_PKPixelFormat32bppCMYK)) {
        return F{4, 4, PF::U8, false, false, false, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat64bppCMYK)) {
        return F{4, 4, PF::U16, false, false, false, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat40bppCMYKAlpha)) {
        return F{4, 5, PF::U8, false, false, true, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat80bppCMYKAlpha)) {
        return F{4, 5, PF::U16, false, false, true, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat32bppCMYKDIRECT)) {
        return F{4, 4, PF::U8, false, false, false, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat64bppCMYKDIRECT)) {
        return F{4, 4, PF::U16, false, false, false, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat40bppCMYKDIRECTAlpha)) {
        return F{4, 5, PF::U8, false, false, true, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat80bppCMYKDIRECTAlpha)) {
        return F{4, 5, PF::U16, false, false, true, false, false, CM::CMYK};
    }

    // n-Channel
    else if (eq(GUID_PKPixelFormat24bpp3Channels)) {
        return F{3, 3, PF::U8, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat48bpp3Channels)) {
        return F{3, 3, PF::U16, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat32bpp4Channels)) {
        return F{4, 4, PF::U8, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp4Channels)) {
        return F{4, 4, PF::U16, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat40bpp5Channels)) {
        return F{5, 5, PF::U8, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat80bpp5Channels)) {
        return F{5, 5, PF::U16, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat48bpp6Channels)) {
        return F{6, 6, PF::U8, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat96bpp6Channels)) {
        return F{6, 6, PF::U16, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat56bpp7Channels)) {
        return F{7, 7, PF::U8, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat112bpp7Channels)) {
        return F{7, 7, PF::U16, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp8Channels)) {
        return F{8, 8, PF::U8, false, false, false, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat128bpp8Channels)) {
        return F{8, 8, PF::U16, false, false, false, false, false, CM::NChannel};
    }

    else if (eq(GUID_PKPixelFormat32bpp3ChannelsAlpha)) {
        return F{3, 4, PF::U8, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp3ChannelsAlpha)) {
        return F{3, 4, PF::U16, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat40bpp4ChannelsAlpha)) {
        return F{4, 5, PF::U8, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat80bpp4ChannelsAlpha)) {
        return F{4, 5, PF::U16, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat48bpp5ChannelsAlpha)) {
        return F{5, 6, PF::U8, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat96bpp5ChannelsAlpha)) {
        return F{5, 6, PF::U16, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat56bpp6ChannelsAlpha)) {
        return F{6, 7, PF::U8, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat112bpp6ChannelsAlpha)) {
        return F{6, 7, PF::U16, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp7ChannelsAlpha)) {
        return F{7, 8, PF::U8, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat128bpp7ChannelsAlpha)) {
        return F{7, 8, PF::U16, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat72bpp8ChannelsAlpha)) {
        return F{8, 9, PF::U8, false, false, true, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat144bpp8ChannelsAlpha)) {
        return F{8, 9, PF::U16, false, false, true, false, false, CM::NChannel};
    }

    // YCC / YUV, subsampled and full
    else if (eq(GUID_PKPixelFormat12bppYCC420)) {
        return F{3, 3, PF::U8, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat16bppYCC422)) {
        return F{3, 3, PF::U8, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat20bppYCC422)) {
        return F{3, 3, PF::U16, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat32bppYCC422)) {
        return F{3, 3, PF::U16, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat24bppYCC444)) {
        return F{3, 3, PF::U8, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat30bppYCC444)) {
        return F{3, 3, PF::U16, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat48bppYCC444)) {
        return F{3, 3, PF::U16, false, false, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat16bpp48bppYCC444FixedPoint)) {
        return F{3, 3, PF::U16, false, true, false, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat20bppYCC420Alpha)) {
        return F{3, 4, PF::U8, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat24bppYCC422Alpha)) {
        return F{3, 4, PF::U8, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat30bppYCC422Alpha)) {
        return F{3, 4, PF::U16, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat48bppYCC422Alpha)) {
        return F{3, 4, PF::U16, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat32bppYCC444Alpha)) {
        return F{3, 4, PF::U8, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat40bppYCC444Alpha)) {
        return F{3, 4, PF::U16, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat64bppYCC444Alpha)) {
        return F{3, 4, PF::U16, false, false, true, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat64bppYCC444AlphaFixedPoint)) {
        return F{3, 4, PF::U16, false, true, true, false, false, CM::YCC};
    }

    return nullopt; // unknown GUID
}

struct IStreamWmpStream {
    WMPStream stream;
    istream* is = nullptr;
    streampos initialPos = 0;
};

ERR iStreamRead(WMPStream* pStream, void* pv, size_t cb) {
    auto* self = reinterpret_cast<IStreamWmpStream*>(pStream);
    size_t totalRead = 0;
    while (self->is && totalRead < cb) {
        self->is->read(reinterpret_cast<char*>(pv) + totalRead, cb - totalRead);
        const auto got = self->is->gcount();
        if (got <= 0) {
            break;
        }

        totalRead += static_cast<size_t>(got);
    }

    return totalRead == cb ? WMP_errSuccess : WMP_errFileIO;
}

ERR iStreamWrite(WMPStream*, const void*, size_t) { return WMP_errFileIO; } // decode only

ERR iStreamSetPos(WMPStream* pStream, size_t offPos) {
    auto* self = reinterpret_cast<IStreamWmpStream*>(pStream);
    self->is->clear();
    self->is->seekg(self->initialPos + static_cast<streamoff>(offPos), ios::beg);
    return self->is->good() ? WMP_errSuccess : WMP_errFileIO;
}

ERR iStreamGetPos(WMPStream* pStream, size_t* poffPos) {
    auto* self = reinterpret_cast<IStreamWmpStream*>(pStream);
    const auto pos = self->is->tellg() - self->initialPos;
    if (pos < 0) {
        return WMP_errFileIO;
    }

    *poffPos = static_cast<size_t>(pos);
    return WMP_errSuccess;
}

Bool iStreamEos(WMPStream* pStream) {
    auto* self = reinterpret_cast<IStreamWmpStream*>(pStream);
    return self->is->eof() ? TRUE : FALSE;
}

ERR iStreamClose(WMPStream**) { return WMP_errSuccess; } // we own the istream's lifetime, nothing to do

} // namespace

Task<vector<ImageData>> JxrImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    const auto initialPos = iStream.tellg();

    // JXR/HD Photo files start with the TIFF-like magic "II" followed by 0x00BC (little endian) or the big-endian variant. Sniff the first
    // bytes and bail early if this clearly isn't a JXR so other loaders get a chance.
    char magic[4] = {0};
    iStream.read(magic, sizeof(magic));
    iStream.clear();
    iStream.seekg(initialPos, ios::beg);

    const bool isJxrLE = magic[0] == 'I' && magic[1] == 'I' && (uint8_t)magic[2] == 0xBC;
    if (!isJxrLE) {
        throw FormatNotSupported{"File is not a JPEG XR image."};
    }

    // --- Set up codec factory and decoder ---
    PKCodecFactory* codecFactory = nullptr;
    JXR_CHECK(PKCreateCodecFactory(&codecFactory, WMP_SDK_VERSION));
    const auto factoryGuard = ScopeGuard{[&]() {
        if (codecFactory) {
            codecFactory->Release(&codecFactory);
        }
    }};

    PKImageDecode* decoder = nullptr;
    const auto decoderGuard = ScopeGuard{[&]() {
        if (decoder) {
            decoder->Release(&decoder);
        }
    }};

    IStreamWmpStream wmpStream{};
    wmpStream.is = &iStream;
    wmpStream.initialPos = initialPos;
    wmpStream.stream.state.pvObj = nullptr;
    wmpStream.stream.Read = iStreamRead;
    wmpStream.stream.Write = iStreamWrite;
    wmpStream.stream.SetPos = iStreamSetPos;
    wmpStream.stream.GetPos = iStreamGetPos;
    wmpStream.stream.EOS = iStreamEos;
    wmpStream.stream.Close = iStreamClose;

    JXR_CHECK(codecFactory->CreateCodec(&IID_PKImageWmpDecode, (void**)&decoder));
    JXR_CHECK(decoder->Initialize(decoder, &wmpStream.stream));

    int32_t width = 0, height = 0;
    JXR_CHECK(decoder->GetSize(decoder, &width, &height));

    const Vector2i size{width, height};
    if (size.x() <= 0 || size.y() <= 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    PKPixelFormatGUID srcGuid;
    JXR_CHECK(decoder->GetPixelFormat(decoder, &srcGuid));

    const auto srcDesc = describeFormat(srcGuid);
    if (!srcDesc) {
        throw ImageLoadError{"Unrecognized JXR source pixel format"};
    }

    const auto outGuid = srcDesc->target.value_or(srcGuid);
    const auto outDesc = *describeFormat(outGuid);

    const bool hasAlpha = outDesc.hasAlpha;
    const auto alphaKind = hasAlpha ? (outDesc.hasPremultipliedAlpha ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight) :
                                      EAlphaKind::None;
    const auto numColorChannels = outDesc.numColorChannels;
    const auto numChannels = hasAlpha ? numColorChannels + 1 : numColorChannels;
    const auto numPixels = posProd(size);

    tlog::debug(
        "JXR source: numChannels={} pixelFormat={} color={} float={} fixed={} bgr={} alpha={}",
        numChannels,
        toString(srcDesc->pixelFormat),
        toString(srcDesc->colorModel),
        srcDesc->isFloat,
        srcDesc->isFixedPoint,
        srcDesc->bgrOrder,
        hasAlpha
    );

    if (hasAlpha) {
        decoder->WMP.wmiSCP.uAlphaMode = 2; // Decode alpha if present
    }

    if (outDesc.bgrOrder) {
        decoder->WMP.wmiI.bRGB = TRUE; // Decode BGR to RGB
    }

    vector<uint8_t> iccProfile;
    if (uint32_t iccSize = 0; !Failed(decoder->GetColorContext(decoder, nullptr, &iccSize)) && iccSize > 0) {
        iccProfile.resize(iccSize);
        if (Failed(decoder->GetColorContext(decoder, iccProfile.data(), &iccSize))) {
            iccProfile.clear();
        } else {
            tlog::debug("Found ICC color profile of size {} bytes", iccSize);
        }
    }

    // EXIF / XMP live in the decoder's descriptive metadata. (Wiring these through Exif/Xmp helpers is left as a follow-up; the structure
    // mirrors the PNG loader's handleExifData / XMP handling.)

    const auto numDecodedChannels = outDesc.numChannels;
    const auto numDecodedSamples = numPixels * numDecodedChannels;

    const auto stride = static_cast<uint32_t>(size.x()) * numDecodedChannels * nBytes(outDesc.pixelFormat);
    auto buf = PixelBuffer::alloc(numDecodedSamples, outDesc.pixelFormat);

    PKRect rect{0, 0, size.x(), size.y()};
    if (IsEqualGUID(srcGuid, outGuid)) {
        JXR_CHECK(decoder->Copy(decoder, &rect, buf.dataBytes(), static_cast<uint32_t>(stride)));
    } else {
        PKFormatConverter* converter = nullptr;
        JXR_CHECK(codecFactory->CreateFormatConverter(&converter));
        const auto converterGuard = ScopeGuard{[&]() {
            if (converter) {
                converter->Release(&converter);
            }
        }};

        // The converter is quite limited in what it can do. We use it only for bit expansion. See the mapping from srcGuid to outGuid above.
        JXR_CHECK(converter->Initialize(converter, decoder, nullptr, outGuid));
        JXR_CHECK(converter->Copy(converter, &rect, buf.dataBytes(), static_cast<uint32_t>(stride)));
    }

    vector<ImageData> result(1);
    auto& resultData = result.front();
    resultData.hasPremultipliedAlpha = alphaKind == EAlphaKind::PremultipliedNonlinear || alphaKind == EAlphaKind::Premultiplied;

    const auto desiredPixelFormat = nBits(outDesc.pixelFormat) > 16 ? EPixelFormat::F32 : EPixelFormat::F16;

    if (outDesc.colorModel == EJxrColorModel::NChannel) {
        resultData.channels = makeNChannels(numColorChannels, size, EPixelFormat::F32, desiredPixelFormat, resultData.partName);
        if (hasAlpha) {
            resultData.channels.emplace_back(Channel::joinIfNonempty(resultData.partName, "A"), size, EPixelFormat::F32, desiredPixelFormat);
        }
    } else {
        resultData.channels = co_await makeInterleavedChannels(
            numChannels, nextSupportedTextureChannelCount(numChannels), hasAlpha, size, EPixelFormat::F32, desiredPixelFormat, resultData.partName, priority
        );
    }

    const auto dstView = MultiChannelView<float>{resultData.channels};

    const auto convertToFloat32 = [&]<bool TO_SRGB = false, bool MULT_ALPHA = false>() -> Task<void> {
        switch (outDesc.pixelFormat) {
            case EPixelFormat::U8:
                co_await toFloat32<TO_SRGB, MULT_ALPHA>(buf.span<const uint8_t>(), numDecodedChannels, dstView, alphaKind, priority);
                break;
            case EPixelFormat::U16:
                co_await toFloat32<TO_SRGB, MULT_ALPHA>(buf.span<const uint16_t>(), numDecodedChannels, dstView, alphaKind, priority);
                break;
            case EPixelFormat::I16:
                // JXR's 16-bit fixed-point format is Q3.13, so we scale by 1/(2^13) to convert to float.
                co_await toFloat32<false, MULT_ALPHA>(
                    buf.span<const int16_t>(), numDecodedChannels, dstView, alphaKind, priority, 1.0f / (float)(1ull << 13)
                );
                break;
            case EPixelFormat::I32:
                // JXR's 32-bit fixed-point format is Q8.24, so we scale by 1/(2^24) to convert to float.
                co_await toFloat32<false, MULT_ALPHA>(
                    buf.span<const int32_t>(), numDecodedChannels, dstView, alphaKind, priority, 1.0f / (float)(1ull << 24)
                );
                break;
            case EPixelFormat::F16:
                co_await toFloat32<false, MULT_ALPHA>(buf.span<const half>(), numDecodedChannels, dstView, alphaKind, priority);
                break;
            case EPixelFormat::F32:
                co_await toFloat32<false, MULT_ALPHA>(buf.span<const float>(), numDecodedChannels, dstView, alphaKind, priority);
                break;
            default: throw ImageLoadError{fmt::format("Unsupported JXR pixel format {}", toString(outDesc.pixelFormat))};
        }
    };

    resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;

    auto rgbaOutView = dstView;
    if (outDesc.colorModel == EJxrColorModel::CMYK || outDesc.colorModel == EJxrColorModel::NChannel) {
        co_await resultData.prependRgb(priority);
        rgbaOutView = MultiChannelView<float>{span{resultData.channels}.subspan(0, 3)};

        if (hasAlpha) {
            rgbaOutView.insertView(3, resultData.channels.at(3 + numChannels - 1).view<float>());
        }
    }

    if (!iccProfile.empty()) {
        // Prefer an embedded ICC profile when present, mirroring the PNG loader's iCCP path.
        try {
            const auto profile = ColorProfile::fromIcc({iccProfile.data(), iccProfile.size()});
            co_await convertToFloat32();

            // HACK: 10-bit JXR XBOX screenshots are encoded in Rec.2020 PQ (ST.2084) but the ICC profile renders SDR while carrying the
            // color space info in its description.
            const auto desc = profile.description();
            if (desc.find("Rec2020 ST.2084") != string::npos || desc.find("Rec.2020 ST.2084") != string::npos) {
                tlog::debug("Detected Rec.2020 PQ ICC profile name; manually converting.");
                co_await ThreadPool::global().parallelFor(
                    0uz,
                    numPixels,
                    numPixels * numColorChannels,
                    [&](size_t i) {
                        for (uint32_t c = 0; c < numColorChannels; ++c) {
                            dstView[c, i] = ituth273::pqToLinear(dstView[c, i]);
                        }
                    },
                    priority
                );

                resultData.nativeMetadata.transfer = ituth273::ETransfer::PQ;
                resultData.nativeMetadata.chroma = bt2020Chroma();
                resultData.toRec709 = convertColorspaceMatrix(*resultData.nativeMetadata.chroma, rec709Chroma(), resultData.renderingIntent);
                resultData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(*resultData.nativeMetadata.transfer);
                co_return result;
            }

            co_await toLinearSrgbPremul(profile, alphaKind, dstView, rgbaOutView, nullopt, priority);
            resultData.hasPremultipliedAlpha = true;
            resultData.readMetadataFromIcc(profile);
            co_return result;
        } catch (const runtime_error& e) { tlog::warning("Failed to apply ICC color profile: {}", e.what()); }
    }

    if (outDesc.colorModel == EJxrColorModel::CMYK) {
        TEV_ASSERT(numColorChannels == 4, "CMYK must have 4 color channels");

        co_await convertToFloat32();
        co_await ThreadPool::global().parallelFor(
            0uz,
            numPixels,
            numPixels * numColorChannels,
            [&](size_t i) {
                const auto c = dstView[0, i];
                const auto m = dstView[1, i];
                const auto y = dstView[2, i];
                const auto k = dstView[3, i];

                rgbaOutView[0, i] = (1.0f - c) * (1.0f - k);
                rgbaOutView[1, i] = (1.0f - m) * (1.0f - k);
                rgbaOutView[2, i] = (1.0f - y) * (1.0f - k);
            },
            priority
        );
    } else if (outDesc.colorModel == EJxrColorModel::NChannel) {
        co_await convertToFloat32.operator()<false, true>();

        const auto minNumChannels = std::min(numColorChannels, 3uz);
        co_await ThreadPool::global().parallelFor(
            0uz,
            numPixels,
            numPixels * minNumChannels,
            [&](size_t i) {
                for (size_t c = 0; c < minNumChannels; ++c) {
                    rgbaOutView[c, i] = dstView[c, i];
                }
            },
            priority
        );

        resultData.hasPremultipliedAlpha = true;
    } else {
        co_await convertToFloat32.operator()<true, true>();
        resultData.hasPremultipliedAlpha = true;

        resultData.nativeMetadata.transfer = outDesc.pixelFormat == EPixelFormat::F16 || outDesc.pixelFormat == EPixelFormat::F32 ?
            ituth273::ETransfer::Linear :
            ituth273::ETransfer::SRGB;
        resultData.nativeMetadata.chroma = rec709Chroma();
    }

    co_return result;
}

} // namespace tev
