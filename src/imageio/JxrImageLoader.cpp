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
#include <tev/imageio/Exif.h>
#include <tev/imageio/Ifd.h>
#include <tev/imageio/JxrImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <string_view>
#include <vector>

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
    bool hasAlpha = false;       // format actually carries an alpha plane
    bool bgrOrder = false;       // stored BGR(A); needs channel swap if consumed raw
    EJxrColorModel colorModel = EJxrColorModel::RGB;
    bool hasPremultipliedAlpha = false;
    optional<PKPixelFormatGUID> target = nullopt;
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
        return F{1, 1, PF::U8, false, false, CM::RGB, false, GUID_PKPixelFormat8bppGray};
    } else if (eq(GUID_PKPixelFormat8bppGray)) {
        return F{1, 1, PF::U8, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat16bppGray)) {
        return F{1, 1, PF::U16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat16bppGrayHalf)) {
        return F{1, 1, PF::F16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppGrayFloat)) {
        return F{1, 1, PF::F32, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat16bppGrayFixedPoint)) {
        return F{1, 1, PF::I16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppGrayFixedPoint)) {
        return F{1, 1, PF::I32, false, false, CM::RGB};
    }

    // Packed RGB needing unpacking
    else if (eq(GUID_PKPixelFormat16bppRGB555)) {
        return F{3, 3, PF::U8, false, false, CM::RGB, false, GUID_PKPixelFormat24bppRGB};
    } else if (eq(GUID_PKPixelFormat16bppRGB565)) {
        return F{3, 3, PF::U8, false, false, CM::RGB, false, GUID_PKPixelFormat24bppRGB};
    } else if (eq(GUID_PKPixelFormat32bppRGB101010)) {
        return F{3, 3, PF::U16, false, false, CM::RGB, false, GUID_PKPixelFormat48bppRGB};
    } else if (eq(GUID_PKPixelFormat32bppRGBE)) {
        return F{3, 3, PF::F32, false, false, CM::RGB, false, GUID_PKPixelFormat96bppRGBFloat};
    }

    // 8-bit RGB(A)
    else if (eq(GUID_PKPixelFormat24bppRGB)) {
        return F{3, 3, PF::U8, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat24bppBGR)) {
        return F{3, 3, PF::U8, false, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppRGB)) {
        return F{3, 4, PF::U8, false, false, CM::RGB}; // 4th channel is padding
    } else if (eq(GUID_PKPixelFormat32bppBGR)) {
        return F{3, 4, PF::U8, false, true, CM::RGB}; // 4th channel is padding
    } else if (eq(GUID_PKPixelFormat32bppRGBA)) {
        return F{3, 4, PF::U8, true, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppBGRA)) {
        return F{3, 4, PF::U8, true, true, CM::RGB};
    } else if (eq(GUID_PKPixelFormat32bppPRGBA)) {
        return F{3, 4, PF::U8, true, false, CM::RGB, true};
    } else if (eq(GUID_PKPixelFormat32bppPBGRA)) {
        return F{3, 4, PF::U8, true, true, CM::RGB, true};
    }

    // 16-bit RGB(A)
    else if (eq(GUID_PKPixelFormat48bppRGB)) {
        return F{3, 3, PF::U16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBA)) {
        return F{3, 4, PF::U16, true, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppPRGBA)) {
        return F{3, 4, PF::U16, true, false, CM::RGB, true};
    }

    // Fixed point RGB(A)
    else if (eq(GUID_PKPixelFormat48bppRGBFixedPoint)) {
        return F{3, 3, PF::I16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBFixedPoint)) {
        return F{3, 4, PF::I16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBAFixedPoint)) {
        return F{3, 4, PF::I16, true, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat96bppRGBFixedPoint)) {
        return F{3, 3, PF::I32, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBFixedPoint)) {
        return F{3, 4, PF::I32, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBAFixedPoint)) {
        return F{3, 4, PF::I32, true, false, CM::RGB};
    }

    // Half float RGB(A)
    else if (eq(GUID_PKPixelFormat48bppRGBHalf)) {
        return F{3, 3, PF::F16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBHalf)) {
        return F{3, 4, PF::F16, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat64bppRGBAHalf)) {
        return F{3, 4, PF::F16, true, false, CM::RGB};
    }

    // 32-bit float RGB(A)
    else if (eq(GUID_PKPixelFormat96bppRGBFloat)) {
        return F{3, 3, PF::F32, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBFloat)) {
        return F{3, 4, PF::F32, false, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppRGBAFloat)) {
        return F{3, 4, PF::F32, true, false, CM::RGB};
    } else if (eq(GUID_PKPixelFormat128bppPRGBAFloat)) {
        return F{3, 4, PF::F32, true, false, CM::RGB, true};
    }

    // CMYK
    else if (eq(GUID_PKPixelFormat32bppCMYK)) {
        return F{4, 4, PF::U8, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat64bppCMYK)) {
        return F{4, 4, PF::U16, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat40bppCMYKAlpha)) {
        return F{4, 5, PF::U8, true, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat80bppCMYKAlpha)) {
        return F{4, 5, PF::U16, true, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat32bppCMYKDIRECT)) {
        return F{4, 4, PF::U8, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat64bppCMYKDIRECT)) {
        return F{4, 4, PF::U16, false, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat40bppCMYKDIRECTAlpha)) {
        return F{4, 5, PF::U8, true, false, CM::CMYK};
    } else if (eq(GUID_PKPixelFormat80bppCMYKDIRECTAlpha)) {
        return F{4, 5, PF::U16, true, false, CM::CMYK};
    }

    // n-Channel
    else if (eq(GUID_PKPixelFormat24bpp3Channels)) {
        return F{3, 3, PF::U8, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat48bpp3Channels)) {
        return F{3, 3, PF::U16, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat32bpp4Channels)) {
        return F{4, 4, PF::U8, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp4Channels)) {
        return F{4, 4, PF::U16, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat40bpp5Channels)) {
        return F{5, 5, PF::U8, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat80bpp5Channels)) {
        return F{5, 5, PF::U16, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat48bpp6Channels)) {
        return F{6, 6, PF::U8, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat96bpp6Channels)) {
        return F{6, 6, PF::U16, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat56bpp7Channels)) {
        return F{7, 7, PF::U8, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat112bpp7Channels)) {
        return F{7, 7, PF::U16, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp8Channels)) {
        return F{8, 8, PF::U8, false, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat128bpp8Channels)) {
        return F{8, 8, PF::U16, false, false, CM::NChannel};
    }

    else if (eq(GUID_PKPixelFormat32bpp3ChannelsAlpha)) {
        return F{3, 4, PF::U8, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp3ChannelsAlpha)) {
        return F{3, 4, PF::U16, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat40bpp4ChannelsAlpha)) {
        return F{4, 5, PF::U8, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat80bpp4ChannelsAlpha)) {
        return F{4, 5, PF::U16, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat48bpp5ChannelsAlpha)) {
        return F{5, 6, PF::U8, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat96bpp5ChannelsAlpha)) {
        return F{5, 6, PF::U16, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat56bpp6ChannelsAlpha)) {
        return F{6, 7, PF::U8, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat112bpp6ChannelsAlpha)) {
        return F{6, 7, PF::U16, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat64bpp7ChannelsAlpha)) {
        return F{7, 8, PF::U8, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat128bpp7ChannelsAlpha)) {
        return F{7, 8, PF::U16, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat72bpp8ChannelsAlpha)) {
        return F{8, 9, PF::U8, true, false, CM::NChannel};
    } else if (eq(GUID_PKPixelFormat144bpp8ChannelsAlpha)) {
        return F{8, 9, PF::U16, true, false, CM::NChannel};
    }

    // YCC / YUV, subsampled and full
    else if (eq(GUID_PKPixelFormat12bppYCC420)) {
        return F{3, 3, PF::U8, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat16bppYCC422)) {
        return F{3, 3, PF::U8, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat20bppYCC422)) {
        return F{3, 3, PF::U16, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat32bppYCC422)) {
        return F{3, 3, PF::U16, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat24bppYCC444)) {
        return F{3, 3, PF::U8, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat30bppYCC444)) {
        return F{3, 3, PF::U16, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat48bppYCC444)) {
        return F{3, 3, PF::U16, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat16bpp48bppYCC444FixedPoint)) {
        return F{3, 3, PF::U16, false, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat20bppYCC420Alpha)) {
        return F{3, 4, PF::U8, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat24bppYCC422Alpha)) {
        return F{3, 4, PF::U8, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat30bppYCC422Alpha)) {
        return F{3, 4, PF::U16, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat48bppYCC422Alpha)) {
        return F{3, 4, PF::U16, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat32bppYCC444Alpha)) {
        return F{3, 4, PF::U8, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat40bppYCC444Alpha)) {
        return F{3, 4, PF::U16, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat64bppYCC444Alpha)) {
        return F{3, 4, PF::U16, true, false, CM::YCC};
    } else if (eq(GUID_PKPixelFormat64bppYCC444AlphaFixedPoint)) {
        return F{3, 4, PF::U16, true, false, CM::YCC};
    }

    return nullopt; // unknown GUID
}

} // namespace

Task<vector<ImageData>>
    JxrImageLoader::load(istringstream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    const auto buffer = toSpan<const uint8_t>(iStream).subspan(iStream.tellg());

    // JXR/HD Photo files start with the TIFF-like magic "II" followed by 0x00BC (little endian) or the big-endian variant. Sniff the first
    // bytes and bail early if this clearly isn't a JXR so other loaders get a chance.
    const bool isJxrLE = buffer.size() >= 4 && buffer[0] == 'I' && buffer[1] == 'I' && buffer[2] == 0xBC;
    if (!isJxrLE) {
        throw FormatNotSupported{"File is not a JPEG XR image."};
    }

    PKImageDecode* decoder = nullptr;
    const auto decoderGuard = ScopeGuard{[&]() {
        if (decoder) {
            decoder->Release(&decoder);
        }
    }};

    PKCodecFactory* codecFactory = nullptr;
    JXR_CHECK(PKCreateCodecFactory(&codecFactory, WMP_SDK_VERSION));
    const auto codecFactoryGuard = ScopeGuard{[&]() {
        if (codecFactory) {
            codecFactory->Release(&codecFactory);
        }
    }};

    JXR_CHECK(codecFactory->CreateCodec(&IID_PKImageWmpDecode, (void**)&decoder));

    PKFactory* factory = nullptr;
    JXR_CHECK(PKCreateFactory(&factory, WMP_SDK_VERSION));
    const auto factoryGuard = ScopeGuard{[&]() {
        if (factory) {
            factory->Release(&factory);
        }
    }};

    WMPStream* stream = nullptr;
    JXR_CHECK(factory->CreateStreamFromMemory(&stream, const_cast<uint8_t*>(buffer.data()), static_cast<uint32_t>(buffer.size())));
    JXR_CHECK(decoder->Initialize(decoder, stream));

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

    optional<uint16_t> colorSpace = nullopt;

    struct PtmColorInfo {
        uint8_t primaries;
        uint8_t transfer;
        uint8_t matrixCoeffs;
        uint8_t fullRangeFlag;
    };
    optional<PtmColorInfo> ptmColorInfo = nullopt;

    try {
        const Ifd ifd{buffer, 0, true};
        colorSpace = ifd.tryGet<uint16_t>(0xa001);
        if (const auto ptmData = ifd.dataSpan(0xbc05); ptmData.size() >= sizeof(PtmColorInfo)) {
            ptmColorInfo = PtmColorInfo{};
            memcpy(&ptmColorInfo.value(), ptmData.data(), sizeof(PtmColorInfo));
        }
    } catch (const invalid_argument& e) { tlog::warning("Failed to read JXR IFD metadata: {}", e.what()); }

    tlog::debug(
        "JXR: numChannels={} pixelFormat={} color={} bgr={} alpha={} cs={}",
        numChannels,
        toString(srcDesc->pixelFormat),
        toString(srcDesc->colorModel),
        srcDesc->bgrOrder,
        hasAlpha,
        colorSpace ? to_string(colorSpace.value()) : "0"
    );

    if (ptmColorInfo) {
        tlog::debug(
            "JXR PTM color info: primaries={} transfer={} matrixCoeffs={} fullRangeFlag={}",
            (int)ptmColorInfo->primaries,
            (int)ptmColorInfo->transfer,
            (int)ptmColorInfo->matrixCoeffs,
            (int)ptmColorInfo->fullRangeFlag
        );

        tlog::warning("JXR PTM color info is present, but tev does not support it. The image may be displayed incorrectly.");
    }

    if (hasAlpha) {
        decoder->WMP.wmiSCP.uAlphaMode = 2; // Decode alpha if present
    }

    if (outDesc.bgrOrder) {
        decoder->WMP.wmiI.bRGB = TRUE; // Decode BGR to RGB
    }

    vector<ImageData> result(1);
    auto& resultData = result.front();

    HeapArray<uint8_t> iccProfile;
    if (uint32_t iccSize = 0; !Failed(decoder->GetColorContext(decoder, nullptr, &iccSize)) && iccSize > 0) {
        iccProfile.resize(iccSize);
        if (Failed(decoder->GetColorContext(decoder, iccProfile.data(), &iccSize))) {
            iccProfile = {};
        } else {
            tlog::debug("Found ICC color profile of size {} bytes", iccSize);
        }
    }

    if (uint32_t exifSize = 0; !Failed(PKImageDecode_GetEXIFMetadata_WMP(decoder, nullptr, &exifSize)) && exifSize > 0) {
        HeapArray<uint8_t> exifData(exifSize);
        if (!Failed(PKImageDecode_GetEXIFMetadata_WMP(decoder, exifData.data(), &exifSize))) {
            tlog::debug("Found EXIF data of size {} bytes", exifSize);

            try {
                const auto exif = Exif{exifData};
                resultData.attributes.emplace_back(exif.toAttributes());
            } catch (const invalid_argument& e) { tlog::warning("Failed to read EXIF metadata: {}", e.what()); }
        }
    }

    if (uint32_t xmpSize = 0; !Failed(PKImageDecode_GetXMPMetadata_WMP(decoder, nullptr, &xmpSize)) && xmpSize > 0) {
        HeapArray<uint8_t> xmpData(xmpSize);
        if (!Failed(PKImageDecode_GetXMPMetadata_WMP(decoder, xmpData.data(), &xmpSize))) {
            tlog::debug("Found XMP data of size {} bytes", xmpSize);

            try {
                const string_view xmpDataView = string_view{(const char*)xmpData.data(), xmpData.size()};
                const auto xmp = Xmp{xmpDataView};

                resultData.attributes.emplace_back(xmp.attributes());
            } catch (const invalid_argument& e) { tlog::warning("Failed to read EXIF metadata: {}", e.what()); }
        }
    }

    if (DESCRIPTIVEMETADATA descMeta; !Failed(decoder->GetDescriptiveMetadata(decoder, &descMeta))) {
        auto& meta = resultData.attributes.emplace_back();
        meta.name = "JPEG XR";
        auto& global = meta.children.emplace_back();
        global.name = "Global";

        const auto addField = [&](string_view name, const DPKPROPVARIANT& field) {
            auto& child = global.children.emplace_back();
            child.name = name;
            switch (field.vt) {
                case DPKVT_UI1:
                    child.value = to_string(field.VT.bVal);
                    child.type = "u8";
                    break;
                case DPKVT_UI2:
                    child.value = to_string(field.VT.uiVal);
                    child.type = "u16";
                    break;
                case DPKVT_UI4:
                    child.value = to_string(field.VT.ulVal);
                    child.type = "u32";
                    break;
                case DPKVT_LPSTR:
                    child.value = string{field.VT.pszVal};
                    child.type = "UTF-8";
                    break;
                case DPKVT_LPWSTR:
                    child.value = utf16to8(u16string_view{(char16_t*)field.VT.pwszVal});
                    child.type = "UTF-16";
                    break;
                case DPKVT_EMPTY:
                    child.value = string{"<empty>"};
                    child.type = "<empty>";
                    break;
                case DPKVT_BYREF:
                    child.value = string{"<byref>"};
                    child.type = "u8*";
                    break;
                default:
                    child.value = string{"<unknown>"};
                    child.type = "<unknown>";
                    break;
            }
        };

        addField("Version", descMeta.pvarImageDescription);
        addField("Camera Make", descMeta.pvarCameraMake);
        addField("Camera Model", descMeta.pvarCameraModel);
        addField("Software", descMeta.pvarSoftware);
        addField("Date Time", descMeta.pvarDateTime);
        addField("Artist", descMeta.pvarArtist);
        addField("Copyright", descMeta.pvarCopyright);
        addField("Rating Stars", descMeta.pvarRatingStars);
        addField("Rating Value", descMeta.pvarRatingValue);
        addField("Caption", descMeta.pvarCaption);
        addField("Document Name", descMeta.pvarDocumentName);
        addField("Page Name", descMeta.pvarPageName);
        addField("Page Number", descMeta.pvarPageNumber);
        addField("Host Computer", descMeta.pvarHostComputer);
    }

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

    // Strictly speaking, the JXR spec only defines COLOR_SPACE values 1 (sRGB) and FFFF (unspecified), see page 158, but in practice some
    // JXR files use 2084 to indicate Rec.2020 PQ transfer function / BT.2020 primaries.
    if (outDesc.colorModel == EJxrColorModel::RGB && colorSpace == 2084) {
        tlog::debug("Detected Rec.2020 PQ transfer function from JXR metadata");

        co_await convertToFloat32();
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

    if (iccProfile) {
        // Prefer an embedded ICC profile when present, mirroring the PNG loader's iCCP path.
        try {
            const auto profile = ColorProfile::fromIcc({iccProfile.data(), iccProfile.size()});
            co_await convertToFloat32();

            co_await toLinearSrgbPremul(profile, alphaKind, dstView, rgbaOutView, nullopt, priority);
            resultData.hasPremultipliedAlpha = true;
            resultData.readMetadataFromIcc(profile);
            co_return result;
        } catch (const runtime_error& e) { tlog::warning("Failed to apply ICC color profile: {}", e.what()); }
    }

    if (outDesc.colorModel == EJxrColorModel::CMYK) {
        TEV_ASSERT(numColorChannels == 4, "CMYK must have 4 color channels");

        // TODO: use SWOP2006_Coated3v2.icc as fallback per JXR spec page 181/182. Need to embed it in tev first, but 2.6 MB...
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
        if (colorSpace == 1) {
            tlog::info("Detected sRGB transfer function from JXR metadata");
            co_await convertToFloat32.operator()<true, true>();
        } else {
            co_await convertToFloat32.operator()<false, true>();
        }

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
        // Regardless of COLOR_SPACE value, RGB unsigned should be treated as sRGB per JXR spec page 181
        co_await convertToFloat32.operator()<true, true>();
        resultData.hasPremultipliedAlpha = true;

        const bool isScRgb = isFloat(outDesc.pixelFormat) || isSignedInt(outDesc.pixelFormat);
        resultData.nativeMetadata.transfer = isScRgb ? ituth273::ETransfer::Linear : ituth273::ETransfer::SRGB;
        resultData.nativeMetadata.chroma = rec709Chroma();
    }

    co_return result;
}

} // namespace tev
