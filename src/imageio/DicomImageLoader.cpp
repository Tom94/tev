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

#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/DicomImageLoader.h>
#include <tev/imageio/ImageLoader.h>

#include <gdcmAttribute.h>
#include <gdcmDataSet.h>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>
#include <gdcmImage.h>
#include <gdcmImageChangePlanarConfiguration.h>
#include <gdcmImageHelper.h>
#include <gdcmImageReader.h>
#include <gdcmMediaStorage.h>
#include <gdcmPhotometricInterpretation.h>
#include <gdcmPixelFormat.h>
#include <gdcmRescaler.h>
#include <gdcmSequenceOfItems.h>
#include <gdcmTag.h>
#include <gdcmTransferSyntax.h>
#include <gdcmVR.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

enum class EDicomKind {
    U8 = 0,
    I8,
    U16,
    I16,
    U32,
    I32,
    U64,
    I64,
    F16,
    F32,
    F64,
};

string toString(EDicomKind kind) {
    switch (kind) {
        case EDicomKind::U8: return "U8";
        case EDicomKind::I8: return "I8";
        case EDicomKind::U16: return "U16";
        case EDicomKind::I16: return "I16";
        case EDicomKind::U32: return "U32";
        case EDicomKind::I32: return "I32";
        case EDicomKind::U64: return "U64";
        case EDicomKind::I64: return "I64";
        case EDicomKind::F16: return "F16";
        case EDicomKind::F32: return "F32";
        case EDicomKind::F64: return "F64";
        default: throw runtime_error{"Unknown DICOM kind."};
    }
}

EDicomKind gdcmScalarToKind(gdcm::PixelFormat::ScalarType st) {
    switch (st) {
        case gdcm::PixelFormat::UINT8: return EDicomKind::U8;
        case gdcm::PixelFormat::INT8: return EDicomKind::I8;
        // 12-bit stored data is unpacked by GDCM into 16-bit containers, so we treat UINT12/INT12 as their 16-bit counterparts.
        case gdcm::PixelFormat::UINT12:
        case gdcm::PixelFormat::UINT16: return EDicomKind::U16;
        case gdcm::PixelFormat::INT12:
        case gdcm::PixelFormat::INT16: return EDicomKind::I16;
        case gdcm::PixelFormat::UINT32: return EDicomKind::U32;
        case gdcm::PixelFormat::INT32: return EDicomKind::I32;
        case gdcm::PixelFormat::UINT64: return EDicomKind::U64;
        case gdcm::PixelFormat::INT64: return EDicomKind::I64;
        case gdcm::PixelFormat::FLOAT16: return EDicomKind::F16;
        case gdcm::PixelFormat::FLOAT32: return EDicomKind::F32;
        case gdcm::PixelFormat::FLOAT64: return EDicomKind::F64;
        default: throw ImageLoadError{fmt::format("Unsupported DICOM scalar type: {}", (int)st)};
    }
}

// Small helpers to read DICOM attributes by tag. GDCM's typed Attribute<> is the safe route; we fall back to gracefully missing values.
template <uint16_t Group, uint16_t Element, typename T> optional<T> dicomGetValue(const gdcm::DataSet& ds, size_t index = 0) {
    const gdcm::Tag tag{Group, Element};
    if (!ds.FindDataElement(tag) || ds.GetDataElement(tag).IsEmpty()) {
        return nullopt;
    }

    gdcm::Attribute<Group, Element> at;
    at.SetFromDataSet(ds);
    if (index >= at.GetNumberOfValues()) {
        return nullopt;
    }

    return static_cast<T>(at.GetValues()[index]);
}

// Number of values in a (possibly multi-valued) attribute; used for windowing, which can carry multiple presets.
template <uint16_t Group, uint16_t Element> size_t dicomNumValues(const gdcm::DataSet& ds) {
    const gdcm::Tag tag{Group, Element};
    if (!ds.FindDataElement(tag) || ds.GetDataElement(tag).IsEmpty()) {
        return 0;
    }

    gdcm::Attribute<Group, Element> at;
    at.SetFromDataSet(ds);
    return at.GetNumberOfValues();
}

// Dispatch over scalar type, since GetBuffer hands us raw bytes.
Task<void> dicomBufferToFloat32(
    EDicomKind kind, span<const uint8_t> bytes, size_t numComponents, const MultiChannelView<float>& view, EAlphaKind alphaKind, int priority
) {
    const auto reinterpret = [&]<typename T>() { return span<const T>{reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)}; };

    switch (kind) {
        case EDicomKind::U8: co_await toFloat32(reinterpret.operator()<uint8_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::I8: co_await toFloat32(reinterpret.operator()<int8_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::U16: co_await toFloat32(reinterpret.operator()<uint16_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::I16: co_await toFloat32(reinterpret.operator()<int16_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::U32: co_await toFloat32(reinterpret.operator()<uint32_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::I32: co_await toFloat32(reinterpret.operator()<int32_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::U64: co_await toFloat32(reinterpret.operator()<uint64_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::I64: co_await toFloat32(reinterpret.operator()<int64_t>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::F16: co_await toFloat32(reinterpret.operator()<half>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::F32: co_await toFloat32(reinterpret.operator()<float>(), numComponents, view, alphaKind, priority, 1.0f); break;
        case EDicomKind::F64: co_await toFloat32(reinterpret.operator()<double>(), numComponents, view, alphaKind, priority, 1.0f); break;
        default: throw ImageLoadError{fmt::format("Unsupported DICOM kind: {}", toString(kind))};
    }
}

// Holds the bits we need to (optionally) reproduce the DICOM display pipeline. Each member maps directly to a DICOM attribute.
struct DicomPixelModule {
    uint16_t bitsAllocated = 16;
    uint16_t bitsStored = 16;
    uint16_t highBit = 15;
    uint16_t pixelRepresentation = 0; // 0 = unsigned, 1 = signed
    uint16_t samplesPerPixel = 1;
    uint16_t planarConfiguration = 0; // 0 = interleaved (RGBRGB), 1 = planar (RRR..GGG..BBB)

    // Modality LUT / rescale (stage 2). Identity by default.
    double rescaleSlope = 1.0;
    double rescaleIntercept = 0.0;

    // VOI / windowing (stage 3). Optional.
    optional<double> windowCenter;
    optional<double> windowWidth;
};

DicomPixelModule readPixelModule(const gdcm::DataSet& ds, const gdcm::Image& image) {
    DicomPixelModule m;

    const auto& pf = image.GetPixelFormat();
    m.bitsAllocated = pf.GetBitsAllocated();
    m.bitsStored = pf.GetBitsStored();
    m.highBit = pf.GetHighBit();
    m.pixelRepresentation = pf.GetPixelRepresentation();
    m.samplesPerPixel = pf.GetSamplesPerPixel();
    m.planarConfiguration = (uint16_t)image.GetPlanarConfiguration();

    // Window Center / Width (0028,1050)/(0028,1051). May be multi-valued; take the first preset if present.
    m.windowCenter = dicomGetValue<0x0028, 0x1050, double>(ds);
    m.windowWidth = dicomGetValue<0x0028, 0x1051, double>(ds);

    return m;
}

// Stage 2: stored -> physical. Optional. Returns true if anything was applied.
Task<bool> applyModalityRescale(float slope, float intercept, const MultiChannelView<float>& view, int priority) {
    if (slope == 1.0f && intercept == 0.0f) {
        co_return false;
    }

    const auto numPixels = posProd(view.size());

    co_await ThreadPool::global().parallelFor(
        0uz,
        numPixels,
        numPixels * view.nChannels(),
        [&](size_t i) {
            for (size_t c = 0; c < view.nChannels(); ++c) {
                view[c, i] = view[c, i] * slope + intercept;
            }
        },
        priority
    );

    co_return true;
}

// Stage 3: physical -> display via linear VOI LUT. Optional. Formula straight from DICOM PS3.3 C.11.2.1.2.1.
Task<bool> applyWindowing(const DicomPixelModule& m, const MultiChannelView<float>& view, int priority) {
    if (!m.windowCenter || !m.windowWidth || *m.windowWidth < 1.0) {
        co_return false;
    }

    const float wc = (float)*m.windowCenter;
    const float ww = (float)*m.windowWidth;

    const float invRange = 1.0f / (ww - 1.0f);

    const auto numPixels = posProd(view.size());
    co_await ThreadPool::global().parallelFor(
        0uz,
        numPixels,
        numPixels * view.nChannels(),
        [&](size_t i) {
            for (size_t c = 0; c < (size_t)view.nChannels(); ++c) {
                const float v = view[c, i];
                // NOTE: the spec says to clamp between 0 and 1, but we skip this clamping out of interest to inspect out-of-window values.
                view[c, i] = (v - (wc - 0.5f)) * invRange + 0.5f;
            }
        },
        priority
    );

    co_return true;
}

// Expands a PALETTE COLOR image: stored values are indices into per-channel LUTs carried in the dataset. We read the LUTs from GDCM's
// LookupTable rather than applying them via ImageApplyLookupTable, so we keep control over precision and stay in float.
Task<void>
    expandPalette(EDicomKind kind, span<const uint8_t> bytes, const gdcm::Image& image, const MultiChannelView<float>& view, int priority) {
    const gdcm::LookupTable& lut = image.GetLUT();
    if (view.nChannels() < 3) {
        throw ImageLoadError{"PALETTE COLOR output requires at least 3 channels."};
    }

    // Pull each LUT channel into a normalized float table. GDCM reports the per-channel length and bit depth.
    const auto loadChannel = [&](gdcm::LookupTable::LookupTableType t) -> vector<float> {
        unsigned short length, subscript, bitSize;
        lut.GetLUTDescriptor(t, length, subscript, bitSize);

        const unsigned short bitSample = lut.GetBitSample();

        tlog::debug("Loading LUT channel {}: length={} subscript={} bitSample={} bitSize={}", (int)t, length, subscript, bitSample, bitSize);

        // I don't understand how exactly bitSample and bitSize differ -- inspecting GDCM's code seems nonsensical without having spent too
        // much time looking into the dcm file format. However, the following handling of GDCM's LUT interface is based on inspection of
        // GDCM's GetLUT code and should at least be robust to all valid combinations.

        if (length == 0) {
            throw ImageLoadError{"Empty palette LUT channel."};
        }

        const auto loadLut = [&]<integral T>() -> vector<float> {
            vector<T> raw(length);
            unsigned int numBytes = 0;
            lut.GetLUT(t, reinterpret_cast<unsigned char*>(raw.data()), numBytes);

            const float bitScale = 1.0f / ((1 << bitSize) - 1);
            vector<float> out(length);
            for (unsigned int i = 0; i < length; ++i) {
                out[i] = toLinear(raw[i] * bitScale);
            }

            return out;
        };

        if (bitSample == 8) {
            if (bitSize == 8) {
                return loadLut.operator()<uint8_t>();
            } else if (bitSize == 16) {
                return loadLut.operator()<uint16_t>();
            } else {
                throw ImageLoadError{fmt::format("Unsupported LUT bit size: {}. Only 8 and 16 are supported.", bitSample)};
            }

            return loadLut.operator()<uint8_t>();
        } else if (bitSample == 16) {
            return loadLut.operator()<uint16_t>();
        } else {
            throw ImageLoadError{fmt::format("Unsupported LUT bit sample: {}. Only 8 and 16 are supported.", bitSize)};
        }
    };

    array<vector<float>, 3> palette = {
        loadChannel(gdcm::LookupTable::RED),
        loadChannel(gdcm::LookupTable::GREEN),
        loadChannel(gdcm::LookupTable::BLUE),
    };

    const auto numPixels = posProd(view.size());

    const auto run = [&]<typename T>(span<const T> indices) -> Task<void> {
        co_await ThreadPool::global().parallelFor(
            0uz,
            numPixels,
            numPixels * 3,
            [&](size_t i) {
                const size_t idx = (size_t)std::max(indices[i], (T)0);
                for (size_t c = 0; c < palette.size(); ++c) {
                    const auto& p = palette[c];
                    view[c, i] = p[std::clamp(idx, (size_t)0, p.size() - 1)];
                }
            },
            priority
        );
    };

    if (kind == EDicomKind::U8) {
        co_await run.operator()<uint8_t>({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    } else if (kind == EDicomKind::U16) {
        co_await run.operator()<uint16_t>({reinterpret_cast<const uint16_t*>(bytes.data()), bytes.size() / 2});
    } else if (kind == EDicomKind::I8) {
        co_await run.operator()<int8_t>({reinterpret_cast<const int8_t*>(bytes.data()), bytes.size()});
    } else if (kind == EDicomKind::I16) {
        co_await run.operator()<int16_t>({reinterpret_cast<const int16_t*>(bytes.data()), bytes.size() / 2});
    } else {
        throw ImageLoadError{fmt::format("PALETTE COLOR images must have 8 bit or 16 bit integer indices but has {}.", toString(kind))};
    }
}

string dicomVrToString(gdcm::VR vr) {
    if (vr == gdcm::VR::INVALID) {
        return "UN";
    }

    const char* s = gdcm::VR::GetVRString(vr);
    return s ? string{s} : "UN";
}

// Format a tag as "(gggg,eeee) Name" using GDCM's global dictionary for the human-readable name.
string dicomTagName(const gdcm::Tag& tag) {
    static const auto& global = gdcm::Global::GetInstance();
    static const auto& dicts = global.GetDicts();

    const char* name = dicts.GetDictEntry(tag).GetName();
    // return fmt::format("{} ({:04x},{:04x})", name && name[0] != '\0' ? name : "n/a", tag.GetGroup(), tag.GetElement());
    return name && name[0] != '\0' ? name : "n/a";
}

AttributeNode dicomDataSetToAttributeNode(const gdcm::DataSet& ds, string_view nodeName);

// Recurse into a Sequence of Items (SQ), producing one child node per item.
AttributeNode dicomSequenceToAttributeNode(const gdcm::DataElement& de, string_view nodeName) {
    AttributeNode node;
    node.name = nodeName;
    node.type = "SQ";

    gdcm::SmartPointer<gdcm::SequenceOfItems> sq = de.GetValueAsSQ();
    if (!sq) {
        return node;
    }

    const auto numItems = sq->GetNumberOfItems();
    node.value = fmt::format("{} item{}", numItems, numItems == 1 ? "" : "s");

    for (gdcm::SequenceOfItems::SizeType i = 1; i <= numItems; ++i) {
        const gdcm::Item& item = sq->GetItem(i);
        node.children.emplace_back(dicomDataSetToAttributeNode(item.GetNestedDataSet(), fmt::format("Item {}", i)));
    }

    return node;
}

AttributeNode dicomDataSetToAttributeNode(const gdcm::DataSet& ds, string_view nodeName) {
    AttributeNode result;
    result.name = nodeName;

    for (auto it = ds.Begin(); it != ds.End(); ++it) {
        const gdcm::DataElement& de = *it;
        const gdcm::Tag& tag = de.GetTag();

        // Skip group-length elements; they're structural and carry no user-facing meaning.
        if (tag.GetElement() == 0x0000) {
            continue;
        }

        const gdcm::VR::VRType vr = de.GetVR();

        if (vr == gdcm::VR::SQ || (de.GetValueAsSQ() && de.GetVR() == gdcm::VR::INVALID)) {
            auto seqNode = dicomSequenceToAttributeNode(de, dicomTagName(tag));
            if (!seqNode.children.empty() || !seqNode.value.empty()) {
                result.children.emplace_back(std::move(seqNode));
            }

            continue;
        }

        ostringstream oss;
        if (!de.IsEmpty()) {
            de.GetValue().Print(oss);
        } else {
            oss << "n/a";
        }

        result.children.emplace_back(
            AttributeNode{
                .name = dicomTagName(tag),
                .value = std::move(oss).str(),
                .type = dicomVrToString(vr),
                .children = {},
            }
        );
    }

    return result;
}

struct DicomImageData {
    ImageData imageData;
    optional<int> seriesNumber;
    optional<int> instanceNumber;
    optional<string> seriesUid;
    optional<string> instanceUid;
};

Task<vector<DicomImageData>> readDicomImage(const gdcm::ImageReader& reader, const ImageLoaderSettings& settings, int priority) {
    const gdcm::Image& baseImage = reader.GetImage();
    const gdcm::File& file = reader.GetFile();
    const gdcm::DataSet& ds = file.GetDataSet();

    const AttributeNode dicomAttributes = dicomDataSetToAttributeNode(ds, "Global");

    gdcm::ImageChangePlanarConfiguration planarConverter;
    planarConverter.SetInput(baseImage);
    if (!planarConverter.Change()) {
        throw ImageLoadError{"Failed to convert planar DICOM pixel data to interleaved."};
    }

    const gdcm::Image& image = planarConverter.GetOutput();

    const unsigned int ndim = image.GetNumberOfDimensions();
    if (ndim < 2) {
        throw ImageLoadError{"DICOM image has fewer than 2 dimensions."};
    }

    const unsigned int* dims = image.GetDimensions();
    const Vector2i size{(int)dims[0], (int)dims[1]};
    if (size.x() == 0 || size.y() == 0) {
        throw ImageLoadError{"DICOM image has zero pixels."};
    }

    // Decode the *entire* pixel blob via GDCM. This is the one call where GDCM does the heavy lifting: it resolves the transfer syntax,
    // de-encapsulates fragments, and runs its bundled codec (JPEG, JPEG-LS, JPEG2000, RLE). If you wanted to slot in your own decoders,
    // this is the seam: instead of GetBuffer(), you'd pull the encapsulated fragments and feed them to your JP2K/JPEG path.
    const size_t bufLen = image.GetBufferLength();
    HeapArray<uint8_t> allBytes(bufLen);
    if (!image.GetBuffer((char*)allBytes.data())) {
        throw ImageLoadError{"Failed to decode DICOM pixel data."};
    }

    const DicomPixelModule m = readPixelModule(ds, image);

    const auto pi = image.GetPhotometricInterpretation();
    const bool isPalette = pi == gdcm::PhotometricInterpretation::PALETTE_COLOR;

    // YBR RCT/ICT transforms are codec-internal and specific to J2K. Decoder already returns RGB data for these.
    const bool isRgb = pi == gdcm::PhotometricInterpretation::RGB || pi == gdcm::PhotometricInterpretation::YBR_RCT ||
        pi == gdcm::PhotometricInterpretation::YBR_ICT;
    const bool isYbr = pi == gdcm::PhotometricInterpretation::YBR_FULL || pi == gdcm::PhotometricInterpretation::YBR_FULL_422 ||
        pi == gdcm::PhotometricInterpretation::YBR_PARTIAL_422 || pi == gdcm::PhotometricInterpretation::YBR_PARTIAL_420;

    const EDicomKind kind = gdcmScalarToKind(image.GetPixelFormat().GetScalarType());

    gdcm::RealWorldValueMappingContent rwvmc = {
        .RealWorldValueIntercept = image.GetIntercept(), .RealWorldValueSlope = image.GetSlope(), .CodeValue = "none", .CodeMeaning = "none"
    };
    bool inPhysicalUnits = gdcm::ImageHelper::GetRealWorldValueMappingContent(file, rwvmc);

    // Number of frames (3rd dimension). We load the first frame as the primary part and additional frames as extra parts; this keeps the
    // loader honest about multi-frame CT/MR without forcing volume reconstruction here.
    const size_t numFrames = ndim >= 3 ? std::max(1u, dims[2]) : 1;

    tlog::debug(
        "DICOM info: size={} frames={} bps={}/{} kind={} spp={} pi={} planar={} signed={} slope={} intercept={} window=[{}, {}]",
        size,
        numFrames,
        m.bitsStored,
        m.bitsAllocated,
        toString(kind),
        m.samplesPerPixel,
        toLower(trim(pi.GetString())),
        m.planarConfiguration,
        m.pixelRepresentation,
        rwvmc.RealWorldValueSlope,
        rwvmc.RealWorldValueIntercept,
        m.windowCenter.value_or(0.0),
        m.windowWidth.value_or(0.0)
    );

    // Determine output channel layout. Palette and YBR expand to RGB; monochrome stays single-channel; RGB stays RGB.
    const size_t numColorChannels = (isPalette || isRgb || isYbr) ? 3 : 1;
    const bool hasAlpha = false; // DICOM pixel data does not carry alpha; overlays are separate and out of scope here.
    const auto alphaKind = EAlphaKind::None;
    const size_t numChannels = numColorChannels;

    // Per-frame byte stride into the decoded blob.
    const size_t componentsPerPixel = m.samplesPerPixel;
    const size_t bytesPerComponent = image.GetPixelFormat().GetPixelSize() / std::max<uint16_t>(1, m.samplesPerPixel);
    const size_t frameBytes = (size_t)size.x() * size.y() * componentsPerPixel * bytesPerComponent;

    if (numFrames * frameBytes > allBytes.size()) {
        throw ImageLoadError{fmt::format(
            "DICOM pixel data too small for expected frame count: expected at least {}, got {}", numFrames * frameBytes, allBytes.size()
        )};
    }

    // We only build the primary frame's channels here; multi-frame handling is done by the caller iterating frames as separate parts.
    const size_t numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
    const auto desiredPixelFormat = m.bitsStored > 16 ? EPixelFormat::F32 : EPixelFormat::F16;

    const auto readFrame = [&](size_t frameIdx) -> Task<ImageData> {
        ImageData resultData;
        resultData.partName = numFrames > 1 ? fmt::format("frame.{}", frameIdx) : "";
        resultData.dataWindow = resultData.displayWindow = size;
        resultData.hasPremultipliedAlpha = true;

        if (!dicomAttributes.children.empty()) {
            resultData.attributes.emplace_back(AttributeNode{.name = "DICOM", .value = "", .type = "", .children = {dicomAttributes}});
        }

        resultData.channels = co_await ImageLoader::makeInterleavedChannels(
            numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, desiredPixelFormat, resultData.partName, priority
        );

        const auto view = MultiChannelView<float>{span{resultData.channels}.subspan(0, numChannels)};

        // First frame's bytes.
        const auto frame = span<const uint8_t>{allBytes}.subspan(frameIdx * frameBytes, frameBytes);

        if (isPalette) {
            // Palette indices bypass the numeric pipeline entirely; they map directly to RGB.
            co_await expandPalette(kind, frame, image, view, priority);

            // Palette indices bypass the numeric pipeline entirely; they map directly to RGB.
            resultData.toRec709 = Matrix4f{1.0f};
            resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;

            co_return resultData;
        }

        co_await dicomBufferToFloat32(kind, frame, componentsPerPixel, view, alphaKind, priority);

        // Planar (RRR..GGG..BBB) data needs de-planarization for the multi-component case. For brevity we assume GDCM-interleaved output
        // here (GDCM can be asked to convert planar->interleaved via ImageChangePlanarConfiguration upstream); warn otherwise.
        if (m.planarConfiguration == 1 && componentsPerPixel > 1) {
            tlog::warning("Planar DICOM pixel data encountered; convert with ImageChangePlanarConfiguration before loading.");
        }

        inPhysicalUnits &=
            co_await applyModalityRescale((float)rwvmc.RealWorldValueSlope, (float)rwvmc.RealWorldValueIntercept, view, priority);
        const bool windowed = co_await applyWindowing(m, view, priority);

        // If neither rescale nor windowing ran and the data is integer, the values are still in raw stored magnitude (e.g. 0..4095). Bring
        // them into a sensible [0,1] display range using the stored bit depth.
        if (!windowed && !inPhysicalUnits && kind != EDicomKind::F16 && kind != EDicomKind::F32 && kind != EDicomKind::F64) {
            const float scale = 1.0f / (float)((1ull << m.bitsStored) - 1);
            const auto numPixels = posProd(size);
            co_await ThreadPool::global().parallelFor(
                0uz,
                numPixels,
                numPixels * view.nChannels(),
                [&](size_t i) {
                    for (size_t c = 0; c < (size_t)view.nChannels(); ++c) {
                        view[c, i] = view[c, i] * scale;
                    }
                },
                priority
            );
        }

        // Stage 4: photometric fix-ups.
        if (isYbr) {
            if (pi == gdcm::PhotometricInterpretation::YBR_PARTIAL_420 || pi == gdcm::PhotometricInterpretation::YBR_PARTIAL_422) {
                const auto yRange = limitedRangeForBitsPerSample(m.bitsStored, false);
                const auto cbcrRange = limitedRangeForBitsPerSample(m.bitsStored, true);

                const auto numPixels = posProd(size);
                co_await ThreadPool::global().parallelFor(
                    0uz,
                    numPixels,
                    numPixels * view.nChannels(),
                    [&](size_t i) {
                        for (size_t c = 0; c < (size_t)view.nChannels(); ++c) {
                            const float scale = c == 0 ? yRange.scale : cbcrRange.scale;
                            const float offset = c == 0 ? yRange.offset : cbcrRange.offset;
                            view[c, i] = (view[c, i] - offset) * scale;
                        }
                    },
                    priority
                );
            }

            if (pi == gdcm::PhotometricInterpretation::YBR_RCT) {
                co_await yCbCrToRgbRct<true>(view, priority);
            } else {
                const float offset = pi == gdcm::PhotometricInterpretation::YBR_ICT ?
                    0.0f :
                    (float)(1 << (m.bitsStored - 1)) / (float)((1 << m.bitsStored) - 1);
                co_await yCbCrToRgb<true>(view, priority, Vector2f{offset});
            }
        } else /* if (isRgb) */ { // Applying inverse sRGB transfer even when not RGB to match JPEG exports from professional software
            const auto numPixels = posProd(size);
            co_await ThreadPool::global().parallelFor(
                0uz,
                numPixels,
                numPixels * view.nChannels(),
                [&](size_t i) {
                    for (size_t c = 0; c < (size_t)view.nChannels(); ++c) {
                        float v = view[c, i];
                        if (pi == gdcm::PhotometricInterpretation::MONOCHROME1) {
                            v = 1.0f - v;
                        }

                        view[c, i] = toLinear(v);
                    }
                },
                priority
            );
        }

        if (const auto iccElem = ds.FindDataElement(gdcm::Tag(0x0028, 0x2000)); iccElem) {
            tlog::warning("DICOM ICC profile present (0028,2000); applying not yet wired up.");
        }

        if (isRgb || isYbr) {
            resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
            resultData.toRec709 = Matrix4f{1.0f};
        } else {
            // Raw physical / linear grayscale.
            resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;
            resultData.toRec709 = Matrix4f{1.0f};
        }

        co_return resultData;
    };

    // Series / instance identification for naming parts
    const auto seriesNumber = dicomGetValue<0x0020, 0x0011, int>(ds);
    const auto instanceNumber = dicomGetValue<0x0020, 0x0013, int>(ds);

    const auto seriesUid = dicomGetValue<0x0020, 0x000E, string>(ds);
    const auto instanceUid = dicomGetValue<0x0008, 0x0018, string>(ds);

    vector<DicomImageData> resultData;
    for (size_t i = 0; i < numFrames; ++i) {
        resultData.emplace_back(co_await readFrame(i), seriesNumber, instanceNumber, seriesUid, instanceUid);
    }

    if (resultData.empty()) {
        throw ImageLoadError{"DICOM image has no frames."};
    }

    // Read overlays into additional channels rather than burning them onto the pixel data
    const size_t numOverlays = baseImage.GetNumberOfOverlays();
    for (size_t i = 0; i < numOverlays; ++i) {
        const gdcm::Overlay& ov = baseImage.GetOverlay(i);
        if (ov.IsEmpty()) {
            continue;
        }

        const Vector2i oSize = {ov.GetColumns(), ov.GetRows()};
        const auto oFrames = std::max(ov.GetNumberOfFrames(), 1u);

        // origin is 1-based (row, col) per DICOM; GDCM gives a signed pair
        const short* originData = ov.GetOrigin();
        const auto oOrigin =
            max(Vector2i{originData[1] - 1, originData[0] - 1}, Vector2i{0}); // (col, row) order for easier indexing into pixel data
        const auto oFrameOrigin = ov.GetFrameOrigin() > 0 ? ov.GetFrameOrigin() - 1 : 0;

        HeapArray<char> mask(posProd(oSize) * oFrames);
        ov.GetUnpackBuffer(mask.data(), mask.size());

        vector<ChannelView<float>> overlayViews;
        for (size_t fi = oFrameOrigin, n = std::min(resultData.size(), (size_t)(oFrameOrigin + oFrames)); fi < n; ++fi) {
            overlayViews.emplace_back(resultData.at(fi)
                                          .imageData.channels
                                          .emplace_back(fmt::format("overlay.{}.L", i), size, EPixelFormat::F32, EPixelFormat::F16)
                                          .view<float>());
        }

        const auto view = MultiChannelView<float>{overlayViews};

        TEV_ASSERT(view.nChannels() == oFrames, "Overlay view must have one channel per frame.");

        co_await ThreadPool::global().parallelFor(
            0,
            oSize.y(),
            (size_t)posProd(oSize) * view.nChannels(),
            [&](int y) {
                const int iy = oOrigin.y() + y;
                if (iy < 0 || iy >= size.y()) {
                    return;
                }

                for (int x = 0; x < oSize.x(); ++x) {
                    const int ix = oOrigin.x() + x;
                    for (size_t c = 0; c < view.nChannels(); ++c) {
                        const auto maskIdx = (size_t)c * posProd(oSize) + (size_t)y * oSize.x() + (size_t)x;
                        view[c, ix, iy] = mask[maskIdx] ? 1.0f : 0.0f;
                    }
                }
            },
            priority
        );
    }

    co_return resultData;
}

Task<vector<DicomImageData>> readDicomImage(istringstream& iStream, const ImageLoaderSettings& settings, int priority) {
    gdcm::ImageReader reader;
    reader.SetStream(iStream);
    if (!reader.Read()) {
        throw ImageLoadError{"Failed to parse DICOM image."};
    }

    co_return co_await readDicomImage(reader, settings, priority);
}

Task<vector<DicomImageData>> readDicomDir(const gdcm::File& dirFile, const fs::path& path, const ImageLoaderSettings& settings, int priority) {
    const gdcm::DataSet& dirDs = dirFile.GetDataSet();

    const gdcm::Tag directoryRecordSequence{0x0004, 0x1220};
    if (!dirDs.FindDataElement(directoryRecordSequence)) {
        throw ImageLoadError{"DICOMDIR has no Directory Record Sequence (0004,1220)."};
    }

    gdcm::SmartPointer<gdcm::SequenceOfItems> sq = dirDs.GetDataElement(directoryRecordSequence).GetValueAsSQ();
    if (!sq || sq->GetNumberOfItems() == 0) {
        throw ImageLoadError{"DICOMDIR Directory Record Sequence is empty."};
    }

    // Referenced File IDs are stored relative to the directory that contains the DICOMDIR itself.
    const fs::path baseDir = path.parent_path();

    // Collect referenced files first so we can report a clean error if none are resolvable.
    vector<fs::path> referencedFiles;
    for (gdcm::SequenceOfItems::SizeType i = 1; i <= sq->GetNumberOfItems(); ++i) {
        const gdcm::Item& item = sq->GetItem(i);
        const gdcm::DataSet& recordDs = item.GetNestedDataSet();

        // Referenced File ID (0004,1500): a multi-valued element whose components are the path segments (originally separated by
        // backslashes per the DICOM file-set convention). Records without it (PATIENT/STUDY/SERIES nodes) are skipped.
        const gdcm::Tag referencedFileId{0x0004, 0x1500};
        if (!recordDs.FindDataElement(referencedFileId) || recordDs.GetDataElement(referencedFileId).IsEmpty()) {
            continue;
        }

        gdcm::Attribute<0x0004, 0x1500> fileIdAttr;
        fileIdAttr.SetFromDataSet(recordDs);

        fs::path refPath = baseDir;
        const unsigned int numSegments = fileIdAttr.GetNumberOfValues();
        for (unsigned int s = 0; s < numSegments; ++s) {
            refPath /= trim(fileIdAttr.GetValue(s));
        }

        if (numSegments == 0) {
            continue;
        }

        if (!fs::exists(refPath)) {
            tlog::warning("DICOMDIR references missing file: {}", refPath);
            continue;
        }

        referencedFiles.emplace_back(std::move(refPath));
    }

    if (referencedFiles.empty()) {
        throw ImageLoadError{"DICOMDIR did not reference any loadable files on disk."};
    }

    const auto loadFile = [&](size_t fi) -> Task<vector<DicomImageData>> {
        const fs::path& refPath = referencedFiles[fi];

        istringstream data;

        {
            co_await ThreadPool::blockingIo().enqueueCoroutine(priority);

            ifstream refStream{refPath, ios::binary};
            ostringstream oss;
            oss << refStream.rdbuf();
            data = istringstream{std::move(oss).str()};
        }

        co_await ThreadPool::global().enqueueCoroutine(priority);

        if (!data) {
            tlog::warning("Failed to open DICOMDIR-referenced file: {}", refPath);
            co_return {};
        }

        try {
            co_return co_await readDicomImage(data, settings, priority);
        } catch (const ImageLoadError& e) {
            tlog::warning("Failed to load DICOMDIR-referenced file {}: {}", refPath, e.what());
        } catch (const ImageLoader::FormatNotSupported& e) {
            tlog::warning("DICOMDIR-referenced file {} is not a supported image: {}", refPath, e.what());
        }

        co_return {};
    };

    // Recurse into each referenced file via load(). We prefix each file's parts with a record-derived name so that the resulting multi-part
    // image keeps PATIENT/STUDY/SERIES/INSTANCE provenance distinguishable.
    vector<Task<vector<DicomImageData>>> loadTasks;
    for (size_t fi = 0; fi < referencedFiles.size(); ++fi) {
        loadTasks.emplace_back(ThreadPool::global().enqueueCoroutine(bind(loadFile, fi), priority));
    }

    auto dirResult = co_await awaitAll(loadTasks) | views::join | toVector;
    if (dirResult.empty()) {
        throw ImageLoadError{"DICOMDIR referenced files but none could be loaded."};
    }

    co_return dirResult;
}

void generatePartNames(vector<DicomImageData>& dicomData) {
    struct InstanceUids {
        set<string> uids;
    };

    struct SeriesUids {
        set<string> uids;
        unordered_map<int, InstanceUids> instances;
    };

    unordered_map<int, SeriesUids> series;

    // First pass: count duplicates and build series/instance maps.
    for (const auto& data : dicomData) {
        const int seriesNum = data.seriesNumber.value_or(0);
        const int instanceNum = data.instanceNumber.value_or(0);

        const string seriesUid = data.seriesUid.value_or("");
        const string instanceUid = data.instanceUid.value_or("");

        auto& seriesMeta = series[seriesNum];
        seriesMeta.uids.insert(seriesUid);

        auto& instanceMeta = seriesMeta.instances[instanceNum];
        instanceMeta.uids.insert(instanceUid);
    }

    for (auto& data : dicomData) {
        const int seriesNum = data.seriesNumber.value_or(0);
        const int instanceNum = data.instanceNumber.value_or(0);

        const string seriesUid = data.seriesUid.value_or("");
        const string instanceUid = data.instanceUid.value_or("");

        const auto& su = series.at(seriesNum);
        const auto& iu = su.instances.at(instanceNum);

        const string seriesPart = su.uids.size() > 1 ?
            fmt::format("series.{}.{}", seriesNum, distance(su.uids.begin(), su.uids.find(seriesUid))) :
            fmt::format("series.{}", seriesNum);
        const string instancePart = iu.uids.size() > 1 ?
            fmt::format("inst.{}.{}", instanceNum, distance(iu.uids.begin(), iu.uids.find(instanceUid))) :
            fmt::format("inst.{}", instanceNum);

        data.imageData.partName = Channel::joinIfNonempty(seriesPart, instancePart);
    }
}

Task<vector<ImageData>>
    DicomImageLoader::load(istringstream& iStream, const fs::path& path, string_view, const ImageLoaderSettings& settings, int priority) const {
    char header[132] = {0};
    iStream.read(reinterpret_cast<char*>(header), sizeof(header));
    const bool hasMagic = header[128] == 'D' && header[129] == 'I' && header[130] == 'C' && header[131] == 'M';

    if (!iStream || !hasMagic) {
        const auto lowerExt = toLower(toString(path.extension()));
        if (lowerExt == ".acr" || lowerExt == ".dcm" || lowerExt == ".dicom") {
            tlog::warning("File has DICOM extension \"{}\" but missing magic; attempting lenient parse.", lowerExt);
        } else {
            throw FormatNotSupported{"File is not a DICOM image."};
        }
    }

    iStream.clear();
    iStream.seekg(0);

    vector<DicomImageData> result;

    try {
        gdcm::ImageReader reader;
        reader.SetStream(iStream);
        if (reader.Read()) {
            result = co_await readDicomImage(reader, settings, priority);
        } else {
            // If we can't read an image this might still be a DICOMDIR. A DICOMDIR is a special DICOM file whose only job is to index other
            // files sitting alongside it on disk. It carries no pixel data of its own; instead its Directory Record Sequence (0004,1220)
            // enumerates PATIENT/STUDY/SERIES/IMAGE records, each IMAGE record pointing at a Referenced File ID (0004,1500). We detect that
            // case here, walk the records, resolve each referenced file relative to the DICOMDIR's location, and recursively load them.
            const gdcm::File& dirFile = reader.GetFile();
            gdcm::MediaStorage ms;
            ms.SetFromFile(dirFile);

            if (ms == gdcm::MediaStorage::MediaStorageDirectoryStorage) {
                tlog::debug("DICOMDIR detected; loading referenced files.");
                result = co_await readDicomDir(dirFile, path, settings, priority);

                generatePartNames(result);
                sort(result.begin(), result.end(), [](const DicomImageData& a, const DicomImageData& b) {
                    return naturalCompare(a.imageData.partName, b.imageData.partName);
                });
            } else {
                throw ImageLoadError{"File is a DICOM file but neither a DICOMDIR nor DICOM image."};
            }
        }
    } catch (const gdcm::Exception& e) { throw ImageLoadError{fmt::format("GDCM error: {}", e.what())}; }

    co_return result | views::transform([](DicomImageData& d) { return std::move(d.imageData); }) | toVector;
}

} // namespace tev
