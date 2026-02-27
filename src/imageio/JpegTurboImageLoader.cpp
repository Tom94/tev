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
#include <tev/imageio/GainMap.h>
#include <tev/imageio/Ifd.h>
#include <tev/imageio/IsoGainMapMetadata.h>
#include <tev/imageio/JpegTurboImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <jpeglib.h>

#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>>
    JpegTurboImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings& settings, int priority) const {
    const size_t initialPos = iStream.tellg();

    unsigned char header[2] = {0};
    iStream.read(reinterpret_cast<char*>(header), 2);
    if (header[0] != 0xFF || header[1] != 0xD8) {
        throw FormatNotSupported{"File is not a JPEG image."};
    }

    iStream.clear();
    iStream.seekg(initialPos, ios::beg);

    // Read the entire stream into memory and decompress from there. JPEG does not support streaming decompression from iostreams.
    iStream.seekg(0, ios::end);
    const size_t fileSize = iStream.tellg();
    iStream.seekg(initialPos, ios::beg);

    HeapArray<unsigned char> buffer(fileSize);
    iStream.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    unordered_set<ptrdiff_t> seenOffsets; // to avoid processing the same data multiple times

    struct IsoGainmapInfo {
        IsoGainMapMetadata metadata;
        optional<chroma_t> chroma = nullopt;
    };

    struct ImageInfo {
        span<const uint8_t> data = {};
        size_t parentIndex = 0;
        string partName = "";

        optional<Ifd> appleMakerNoteIfd = nullopt;
        optional<IsoGainmapInfo> isoGainmapInfo = nullopt;
        bool isAppleGainmap = false;

        bool isGainmap() const { return isoGainmapInfo || isAppleGainmap; }
    };

    vector<ImageInfo> imageInfos;
    imageInfos.emplace_back(span<const uint8_t>{buffer}, 0);

    const auto decodeJpeg = [priority, &seenOffsets, &imageInfos, &buffer](span<const uint8_t> data, size_t idx) -> Task<ImageData> {
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;

        cinfo.err = jpeg_std_error(&jerr);
        jerr.error_exit = [](j_common_ptr cinfo) {
            char buf[JMSG_LENGTH_MAX];
            cinfo->err->format_message(cinfo, buf);
            throw ImageLoadError{fmt::format("libjpeg error: {}", buf)};
        };

        jpeg_create_decompress(&cinfo);
        const ScopeGuard jpegGuard{[&]() { jpeg_destroy_decompress(&cinfo); }};

        // Set up source manager to read from memory. In the future we might be able to jury-rig this to read directly from the stream.
        jpeg_mem_src(&cinfo, data.data(), data.size());

        struct AppNSpans {
            span<const uint8_t> exif;
            span<const uint8_t> xmp;
            span<const uint8_t> iso;
            span<const uint8_t> mpf;

            vector<span<const uint8_t>> iccChunks;
        } appN;

        cinfo.client_data = &appN;

        const auto processMarker = [](j_decompress_ptr cinfo) -> boolean {
            // Because we're reading from memory, cinfo->src points directly into `buffer`

            // Read marker length (2 bytes, big-endian)
            const uint8_t* data = cinfo->src->next_input_byte;
            const uint16_t length = (*data << 8) | *(data + 1);

            if (length > cinfo->src->bytes_in_buffer) {
                tlog::warning() << "JPEG marker length exceeds buffer size, skipping.";
                return FALSE;
            }

            const auto extractMarker = [&](span<const uint8_t> ns) -> optional<span<const uint8_t>> {
                if (length > ns.size() + 2 && !memcmp(data + 2, ns.data(), ns.size())) {
                    return span<const uint8_t>{data + ns.size() + 2, length - ns.size() - 2};
                } else {
                    return nullopt;
                }
            };

            AppNSpans* appN = static_cast<AppNSpans*>(cinfo->client_data);
            appN->exif = extractMarker(Exif::FOURCC).value_or(appN->exif);

            static constexpr uint8_t xmpNs[] = "http://ns.adobe.com/xap/1.0/";
            appN->xmp = extractMarker({xmpNs, sizeof(xmpNs)}).value_or(appN->xmp);

            static constexpr uint8_t isoNs[] = "urn:iso:std:iso:ts:21496:-1";
            appN->iso = extractMarker({isoNs, sizeof(isoNs)}).value_or(appN->iso);

            static constexpr uint8_t mpfNs[] = "MPF";
            appN->mpf = extractMarker({mpfNs, sizeof(mpfNs)}).value_or(appN->mpf);

            // ICC profile may be split across multiple APP2 markers, each with a sequence number, hence the special handling
            static constexpr uint8_t iccNs[] = "ICC_PROFILE";
            if (const auto iccPart = extractMarker({iccNs, sizeof(iccNs)})) {
                if (iccPart->size() < 2) {
                    tlog::warning() << "ICC profile APP2 marker too short, skipping.";
                } else {
                    const uint8_t seqNo = (*iccPart)[0];
                    const uint8_t numSeq = (*iccPart)[1];

                    tlog::debug() << fmt::format("Found ICC profile part {}/{} of size {} bytes", seqNo, numSeq, iccPart->size());

                    if (numSeq != appN->iccChunks.size() && appN->iccChunks.size() != 0) {
                        tlog::warning()
                            << fmt::format("Inconsistent ICC profile sequence count: expected {}, got {}.", appN->iccChunks.size(), numSeq);
                    }

                    if (seqNo < 1 || seqNo > numSeq) {
                        tlog::warning() << fmt::format("Invalid ICC profile sequence number: {} of {}.", seqNo, numSeq);
                    }

                    appN->iccChunks.resize(numSeq);
                    appN->iccChunks.at(seqNo - 1) = iccPart->subspan(2);
                }
            }

            cinfo->src->next_input_byte += length;
            cinfo->src->bytes_in_buffer -= length;

            return TRUE;
        };

        jpeg_set_marker_processor(&cinfo, JPEG_APP0 + 1, processMarker); // EXIF, XMP
        jpeg_set_marker_processor(&cinfo, JPEG_APP0 + 2, processMarker); // ISO, MPF

        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            throw ImageLoadError{"Failed to read JPEG header."};
        }

        cinfo.quantize_colors = false;
        if (cinfo.jpeg_color_space == JCS_UNKNOWN) {
            cinfo.jpeg_color_space = JCS_RGB;
        }

        cinfo.out_color_space = cinfo.jpeg_color_space; // Keep the original color space, we'll handle color conversion ourselves if needed
        jpeg_start_decompress(&cinfo);
        ScopeGuard decompressGuard{[&]() { jpeg_abort_decompress(&cinfo); }};

        if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
            throw ImageLoadError{"CMYK JPEG images are not supported."};
        }

        Vector2i size{(int)cinfo.output_width, (int)cinfo.output_height};
        if (size.x() == 0 || size.y() == 0) {
            throw ImageLoadError{"Image has zero pixels."};
        }

        if (cinfo.data_precision < 2 || cinfo.data_precision > 16) {
            throw ImageLoadError{fmt::format("Unsupported JPEG data precision: {} bits per channel.", cinfo.data_precision)};
        }

        const auto pixelFormat = cinfo.data_precision <= 8 ? EPixelFormat::U8 : EPixelFormat::U16;

        // JPEG does not support alpha, so all channels are color channels.
        const size_t numChannels = cinfo.output_components;
        if (numChannels > 4) {
            throw ImageLoadError{fmt::format("Unsupported number of color channels: {}", numChannels)};
        }

        const bool hasAlpha = numChannels == 4;
        const auto numColorChannels = numChannels - (hasAlpha ? 1 : 0);

        tlog::debug() << fmt::format("JPEG image info: size={} numColorChannels={} precision={}", size, numChannels, cinfo.data_precision);

        // Allocate memory for image data
        const auto numPixels = static_cast<size_t>(size.x()) * size.y();
        const auto bytesPerSample = nBytes(pixelFormat);
        const auto numBytesPerPixel = numChannels * bytesPerSample;
        Channel::Data imageData(numPixels * numBytesPerPixel);

        // Create row pointers for libjpeg and then read image
        HeapArray<JSAMPROW> rowPointers(size.y());
        for (int y = 0; y < size.y(); ++y) {
            rowPointers[y] = &imageData[y * size.x() * numBytesPerPixel];
        }

        while (cinfo.output_scanline < cinfo.output_height) {
            if (cinfo.data_precision <= 8) {
                jpeg_read_scanlines(&cinfo, &rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
            } else if (cinfo.data_precision <= 12) {
                jpeg12_read_scanlines(&cinfo, (J12SAMPARRAY)&rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
            } else {
                jpeg16_read_scanlines(&cinfo, (J16SAMPARRAY)&rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
            }
        }

        decompressGuard.disarm();
        jpeg_finish_decompress(&cinfo);

        ImageData resultData;

        if (!appN.mpf.empty()) {
            tlog::debug() << fmt::format("Found MPF data of size {} bytes", appN.mpf.size());

            try {
                const auto handleIfd = [&](const Ifd& ifd) {
                    enum class EMpfTag : uint16_t {
                        MPFVersion = 0xB000,
                        // Index tags
                        NumberOfImages = 0xB001,
                        MPEntry = 0xB002,
                        ImageUIDList = 0xB003,
                        TotalFrames = 0xB004,
                        // Attribute tags
                        MPIndividualNum = 0xB101,
                        PanOrientation = 0xB201,
                        PanOverlapH = 0xB202,
                        PanOverlapV = 0xB203,
                        BaseViewpointNum = 0xB204,
                        ConvergenceAngle = 0xB205,
                        BaselineLength = 0xB206,
                        VerticalDivergence = 0xB207,
                        AxisDistanceX = 0xB208,
                        AxisDistanceY = 0xB209,
                        AxisDistanceZ = 0xB20A,
                        YawAngle = 0xB20B,
                        PitchAngle = 0xB20C,
                        RollAngle = 0xB20D,
                    };

                    // TODO: extract metadata from attribute tags if present

                    const uint16_t numImages = ifd.tryGet<uint16_t>((uint16_t)EMpfTag::NumberOfImages).value_or(0);
                    const auto* iiTag = ifd.tag((uint16_t)EMpfTag::MPEntry);
                    if (numImages > 0 && iiTag) {
                        tlog::debug() << fmt::format("MPF number of sub-images: {}", numImages);

                        enum class EMpfImageType : uint32_t {
                            Undefined = 0x000000,
                            LargeThumbnailVga = 0x010001,
                            LargeThumbnailFullHd = 0x010002,
                            MultiFramePanorama = 0x020001,
                            MultiFrameDisparity = 0x020002,
                            MultiFrameMultiAngle = 0x020003,
                            Primary = 0x030000,
                        };

                        const auto mfpTypeToString = [](EMpfImageType type) -> string {
                            switch (type) {
                                case EMpfImageType::Undefined: return "undefined";
                                case EMpfImageType::LargeThumbnailVga: return "large_thumbnail_vga";
                                case EMpfImageType::LargeThumbnailFullHd: return "large_thumbnail_full_hd";
                                case EMpfImageType::MultiFramePanorama: return "multi_frame_panorama";
                                case EMpfImageType::MultiFrameDisparity: return "multi_frame_disparity";
                                case EMpfImageType::MultiFrameMultiAngle: return "multi_frame_multi_angle";
                                case EMpfImageType::Primary: return "primary";
                                default: return "unknown";
                            }
                        };

                        struct ImageInfoEntry {
                            uint32_t attributes;
                            uint32_t size;
                            uint32_t offset;
                            uint16_t dependentImage1EntryNumber;
                            uint16_t dependentImage2EntryNumber;

                            uint8_t flags() const { return (attributes >> 24) & 0xFF; }
                            EMpfImageType type() const { return (EMpfImageType)(attributes & 0x00FFFFFF); }
                        };

                        if (iiTag->data.size() < sizeof(ImageInfoEntry) * numImages) {
                            throw invalid_argument{"MPF: ImageInformationArray too small."};
                        }

                        for (size_t i = 0; i < numImages; ++i) {
                            ImageInfoEntry iie;
                            const auto* iiData = iiTag->data.data() + i * sizeof(iie);
                            iie.attributes = ifd.read<uint32_t>(iiData + 0);
                            iie.size = ifd.read<uint32_t>(iiData + 4);
                            iie.offset = ifd.read<uint32_t>(iiData + 8);
                            iie.dependentImage1EntryNumber = ifd.read<uint16_t>(iiData + 12);
                            iie.dependentImage2EntryNumber = ifd.read<uint16_t>(iiData + 14);

                            tlog::debug() << fmt::format(
                                "  #{}: flags={:02X} type={} size={} offset={} dep1={} dep2={}",
                                i,
                                iie.flags(),
                                mfpTypeToString(iie.type()),
                                iie.size,
                                iie.offset,
                                iie.dependentImage1EntryNumber,
                                iie.dependentImage2EntryNumber
                            );

                            const string partName = fmt::format("{}.{}", mfpTypeToString(iie.type()), idx + i);

                            // Skip images with zero offset: those are the one we're already reading. But: in this case we should overwrite
                            // the part name if we're not the top-level primary image. (Primary image should have empty part name.)
                            if (iie.offset == 0) {
                                const bool isTopLevelPrimary = idx == 0 && iie.type() == EMpfImageType::Primary;
                                if (!isTopLevelPrimary) {
                                    resultData.partName = partName;
                                }

                                continue;
                            }

                            // We aren't interested in cluttering tev with thumbnail images. Generic multiframe images are fine, though
                            if (iie.type() == EMpfImageType::LargeThumbnailVga || iie.type() == EMpfImageType::LargeThumbnailFullHd) {
                                tlog::debug() << fmt::format("Skipping MPF thumbnail image #{}", i);
                                continue;
                            }

                            // The offset is relative to the start of the MPF data
                            const uint8_t* imageData = appN.mpf.data() + iie.offset;
                            const ptrdiff_t imageDataOffset = imageData - buffer.data();
                            if (seenOffsets.find(imageDataOffset) != seenOffsets.end()) {
                                tlog::warning() << fmt::format("Already seen image at offset {}, skipping", imageDataOffset);
                                continue;
                            }

                            const auto slice = span<const uint8_t>{imageData, iie.size};
                            if (slice.data() + slice.size() > buffer.data() + buffer.size()) {
                                tlog::warning() << fmt::format("MPF image #{} exceeds buffer bounds, skipping", i);
                                continue;
                            }

                            tlog::debug()
                                << fmt::format("Adding MPF image #{} slice at offset {} of size {} bytes", i, imageDataOffset, slice.size());

                            imageInfos.emplace_back(slice, idx, partName);
                        }
                    }
                };

                optional<Ifd> ifd = Ifd{appN.mpf, 0, true};
                while (true) {
                    handleIfd(*ifd);
                    if (!ifd->nextIfdOffset().has_value()) {
                        break;
                    }

                    tlog::debug() << fmt::format("Found sub-IFD in MPF data at offset {}", *ifd->nextIfdOffset());
                    ifd = Ifd{appN.mpf, *ifd->nextIfdOffset(), false, ifd->reverseEndianess()};
                }
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read MPF data: {}", e.what()); }
        }

        // Important to take this reference *after* processing the MPF data because that may add entries to imageInfos, which would
        // invalidate references taken beforehand.
        auto& imageInfo = imageInfos.at(idx);

        if (resultData.partName.empty()) {
            resultData.partName = imageInfo.partName;
        }

        // Per ISO 21496-1, an sRGB color space exif setting takes precedence over ICC profiles
        bool forceSrgb = false;
        EOrientation orientation = EOrientation::None;

        if (!appN.exif.empty()) {
            tlog::debug() << fmt::format("Found EXIF data of size {} bytes", appN.exif.size());

            try {
                const auto exif = Exif{appN.exif};
                resultData.attributes.emplace_back(exif.toAttributes());

                forceSrgb = exif.forceSrgb();
                if (forceSrgb) {
                    tlog::debug() << "EXIF forces sRGB color space.";
                }

                const EOrientation exifOrientation = exif.getOrientation();
                if (exifOrientation != EOrientation::None) {
                    orientation = exifOrientation;
                    tlog::debug() << fmt::format("EXIF image orientation: {}", (int)orientation);
                }

                imageInfo.appleMakerNoteIfd = exif.tryGetAppleMakerNote();
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
        }

        optional<IsoGainMapMetadata> isoGainmapMetadata = nullopt;

        if (!appN.xmp.empty()) {
            const string_view xmpDataView = string_view{(const char*)appN.xmp.data(), appN.xmp.size()};
            tlog::debug() << fmt::format("Found XMP data of size {} bytes", xmpDataView.size());

            try {
                const auto xmp = Xmp{xmpDataView};
                resultData.attributes.emplace_back(xmp.attributes());

                const EOrientation xmpOrientation = xmp.orientation();
                if (xmpOrientation != EOrientation::None) {
                    orientation = xmpOrientation;
                    tlog::debug() << fmt::format("XMP image orientation: {}", (int)orientation);
                }

                isoGainmapMetadata = xmp.isoGainMapMetadata();

                if (!xmp.appleAuxImgType().empty()) {
                    tlog::debug() << fmt::format("Found Apple auxiliary image type in XMP: '{}'", xmp.appleAuxImgType());
                    resultData.partName = xmp.appleAuxImgType();
                    ranges::replace(resultData.partName, ':', '.');

                    imageInfo.isAppleGainmap = resultData.partName.find("apple") != string::npos &&
                        resultData.partName.find("hdrgainmap") != string::npos;
                }
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read XMP metadata: {}", e.what()); }
        }

        if (orientation != EOrientation::None) {
            size = co_await orientToTopLeft(pixelFormat, imageData, size, orientation, priority);
        }

        if (!appN.iso.empty()) {
            tlog::debug() << fmt::format("Found binary ISO 21496-1 data of size {} bytes", appN.iso.size());

            try {
                if (appN.iso.size() <= 4) {
                    const auto isoGainmapVersion = IsoGainMapVersion{appN.iso};
                    tlog::debug() << fmt::format("ISO 21496-1 version info only: '{}'", isoGainmapVersion.toString());
                } else {
                    isoGainmapMetadata = IsoGainMapMetadata{appN.iso};
                }
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read ISO 21496-1 version data: {}", e.what()); }
        }

        if (isoGainmapMetadata.has_value()) {
            tlog::debug() << fmt::format("Gain map metadata version '{}'", isoGainmapMetadata->version().toString());
            resultData.attributes.emplace_back(isoGainmapMetadata->toAttributes());
            resultData.partName = "gainmap";
            imageInfo.isoGainmapInfo = make_optional<IsoGainmapInfo>(*isoGainmapMetadata);
        }

        // This JPEG loader is at most 8 bits per channel (technically, JPEG can hold more, but we don't support that here). Thus easily
        // fits into F16.
        const size_t numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
        resultData.channels = co_await makeRgbaInterleavedChannels(
            numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, resultData.partName, priority
        );

        const auto jpegDataToFloat32Typed = [&](auto* src, bool fromSrgb, float* dst, size_t numDstChannels) -> Task<void> {
            using T = remove_const_t<remove_pointer_t<decltype(src)>>;
            const float scale = 1.0f / ((1 << cinfo.data_precision) - 1);

            const bool yCbCrConversionNeeded = cinfo.out_color_space == JCS_YCbCr && numDstChannels >= 3;

            if (fromSrgb && !yCbCrConversionNeeded) {
                co_await toFloat32<T, true>(src, numChannels, dst, numDstChannels, size, hasAlpha, priority, scale);
            } else {
                co_await toFloat32<T, false>(src, numChannels, dst, numDstChannels, size, hasAlpha, priority, scale);
            }

            if (cinfo.out_color_space == JCS_YCbCr && numDstChannels >= 3) {
                const auto dataView = MultiChannelView<float>{dst, numDstChannels, size, numDstChannels};
                if (fromSrgb) {
                    co_await yCbCrToRgb<true>(dataView, priority);
                } else {
                    co_await yCbCrToRgb<false>(dataView, priority);
                }
            }
        };

        const auto jpegDataToFloat32 = [&](bool fromSrgb, float* dst, size_t numDstChannels) -> Task<void> {
            if (pixelFormat == EPixelFormat::U8) {
                co_await jpegDataToFloat32Typed((const uint8_t*)imageData.data(), fromSrgb, dst, numDstChannels);
            } else if (pixelFormat == EPixelFormat::U16) {
                co_await jpegDataToFloat32Typed((const uint16_t*)imageData.data(), fromSrgb, dst, numDstChannels);
            } else {
                throw ImageLoadError{fmt::format("Unsupported pixel format: {}", (int)pixelFormat)};
            }
        };

        // Since JPEG always has no alpha channel, we default to 1, where premultiplied and straight are equivalent.
        resultData.hasPremultipliedAlpha = !hasAlpha;

        // If an ICC profile exists, use it to convert to linear sRGB. Otherwise, assume the decoder gave us sRGB/Rec.709 (per the JPEG
        // spec) and convert it to linear space via inverse sRGB transfer function.
        if (!forceSrgb) {
            vector<uint8_t> iccProfile;
            for (const auto& chunk : appN.iccChunks) {
                iccProfile.insert(iccProfile.end(), chunk.begin(), chunk.end());
            }

            if (!iccProfile.empty()) {
                try {
                    const auto profile = ColorProfile::fromIcc(iccProfile);

                    // Per ISO 21496-1, gain maps should be loaded as-is in their encoded color space (except for the conversion from
                    // YCbCr), and their ICC profile should only be used for its chroma at gain map application time.
                    if (imageInfo.isGainmap()) {
                        if (imageInfo.isoGainmapInfo) {
                            imageInfo.isoGainmapInfo->chroma = profile.chroma();
                        }

                        co_await jpegDataToFloat32(false, resultData.channels.front().floatData(), numInterleavedChannels);
                        co_return resultData;
                    }

                    HeapArray<float> floatData(numPixels * numChannels);
                    co_await jpegDataToFloat32(false, floatData.data(), numChannels);

                    co_await toLinearSrgbPremul(
                        profile,
                        size,
                        numColorChannels,
                        hasAlpha ? EAlphaKind::Straight : EAlphaKind::None,
                        floatData.data(),
                        resultData.channels.front().floatData(),
                        numInterleavedChannels,
                        nullopt,
                        priority
                    );

                    resultData.readMetadataFromIcc(profile);
                    co_return resultData;
                } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
            }
        }

        co_await jpegDataToFloat32(!imageInfo.isGainmap(), resultData.channels.front().floatData(), numInterleavedChannels);

        if (!imageInfo.isGainmap()) {
            resultData.nativeMetadata.chroma = rec709Chroma();
            resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
        }

        co_return resultData;
    };

    vector<ImageData> result;
    vector<int> resultIndices;

    for (size_t i = 0; i < imageInfos.size(); ++i) {
        auto imageData = co_await decodeJpeg(imageInfos[i].data, i);

        // Danger: imageInfos may grow due to decodeJpeg adding MPF images!
        const auto& imageInfo = imageInfos[i];
        if (!imageInfo.isGainmap()) {
            // Non-gainmap images are added directly to the result set and not processed further
            result.emplace_back(std::move(imageData));
            resultIndices.emplace_back(i);
            continue;
        }

        resultIndices.emplace_back(-1);

        if (imageInfo.parentIndex >= resultIndices.size() || resultIndices.at(imageInfo.parentIndex) == -1) {
            tlog::warning() << fmt::format("Gain map image {} has invalid parent index {}, skipping.", i, imageInfo.parentIndex);
            continue;
        }

        if (imageInfo.parentIndex == i) {
            tlog::warning() << fmt::format("Gain map image {} has itself as parent. Skipping.", i);
            continue;
        }

        tlog::debug() << fmt::format("Applying gain map from image {} to parent image {}.", i, imageInfo.parentIndex);

        const auto resultIndex = resultIndices.at(imageInfo.parentIndex);
        const auto& mainImageInfo = imageInfos.at(imageInfo.parentIndex);
        auto& mainImage = result.at(resultIndex);

        // ISO gain maps take precedence over Apple gain maps. Former is a newer standard all big companies agreed on, latter is an older
        // proprietary Apple thing. Many images are dual-encoded for backwards compatibility, so prefer the standardized on in that case.
        if (imageInfo.isoGainmapInfo) {
            const auto& isoMetadata = imageInfo.isoGainmapInfo->metadata;
            mainImage.attributes.emplace_back(isoMetadata.toAttributes());

            co_await preprocessAndApplyIsoGainMap(
                mainImage, imageData, isoMetadata, mainImage.nativeMetadata.chroma, imageInfo.isoGainmapInfo->chroma, settings.gainmapHeadroom, priority
            );
        } else if (imageInfo.isAppleGainmap) {
            co_await preprocessAndApplyAppleGainMap(mainImage, imageData, mainImageInfo.appleMakerNoteIfd, settings.gainmapHeadroom, priority);
        }

        mainImage.channels.insert(
            mainImage.channels.end(), make_move_iterator(imageData.channels.begin()), make_move_iterator(imageData.channels.end())
        );
    }

    co_return result;
}

} // namespace tev
