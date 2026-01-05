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
#include <tev/imageio/JpegTurboImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <jpeglib.h>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {
// Taken from jpegdecoderhelper in libultrahdr per their Apache 2.0 license and shortened.
// https://github.com/google/libultrahdr/blob/6db3a83ee2b1f79850f3f597172289808dc6a331/lib/src/jpegdecoderhelper.cpp#L125
void jpeg_extract_marker_payload(
    const j_decompress_ptr cinfo, const uint32_t markerCode, span<const uint8_t> ns, std::vector<uint8_t>& destination
) {
    for (jpeg_marker_struct* marker = cinfo->marker_list; marker; marker = marker->next) {
        if (marker->marker == markerCode && marker->data_length > ns.size() && !memcmp(marker->data, ns.data(), ns.size())) {
            destination.resize(marker->data_length);
            memcpy(static_cast<void*>(destination.data()), marker->data, marker->data_length);
            return;
        }
    }
}
} // namespace

Task<vector<ImageData>> JpegTurboImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool) const {
    unsigned char header[2] = {0};
    iStream.read(reinterpret_cast<char*>(header), 2);
    if (header[0] != 0xFF || header[1] != 0xD8) {
        throw FormatNotSupported{"File is not a JPEG image."};
    }

    iStream.clear();
    iStream.seekg(0);

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr cinfo) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message)(cinfo, buffer);
        throw ImageLoadError{fmt::format("JPEG error: {}", buffer)};
    };

    jpeg_create_decompress(&cinfo);
    ScopeGuard jpegGuard{[&]() { jpeg_destroy_decompress(&cinfo); }};

    // Read the entire stream into memory and decompress from there. JPEG does not support streaming decompression from iostreams.
    iStream.seekg(0, std::ios::end);
    size_t fileSize = iStream.tellg();
    iStream.seekg(0, std::ios::beg);

    std::vector<unsigned char> buffer(fileSize);
    iStream.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    // Set up source manager to read from memory. In the future we might be able to jury-rig this to read directly from the stream.
    jpeg_mem_src(&cinfo, buffer.data(), buffer.size());

    // Required call before jpeg_read_header for reading metadata
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF); // EXIF, XMP
    jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF); // ICC, ISO

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        throw ImageLoadError{"Failed to read JPEG header."};
    }

    vector<uint8_t> exifData;
    jpeg_extract_marker_payload(&cinfo, JPEG_APP0 + 1, Exif::FOURCC, exifData);

    vector<uint8_t> xmpData;
    static constexpr string_view xmpNs = "http://ns.adobe.com/xap/1.0/";
    jpeg_extract_marker_payload(&cinfo, JPEG_APP0 + 1, {(const uint8_t*)xmpNs.data(), xmpNs.size()}, xmpData);

    // Try to extract an ICC profile for correct color space conversion
    JOCTET* iccProfile = nullptr;
    unsigned int iccProfileSize = 0;
    if (jpeg_read_icc_profile(&cinfo, &iccProfile, &iccProfileSize) == TRUE) {
        tlog::debug() << fmt::format("Found ICC color profile of size {} bytes", iccProfileSize);
    }

    // The ICC profile appears to be the only thing that we need to free ourselves. All other jpeg related data is managed by
    // libjpeg-turbo's memory manager. See libjpeg-turbo/doc/libjpeg.txt
    ScopeGuard iccGuard{[&]() {
        if (iccProfile) {
            free(iccProfile);
        }
    }};

    cinfo.quantize_colors = false;

    jpeg_start_decompress(&cinfo);

    Vector2i size{static_cast<int>(cinfo.output_width), static_cast<int>(cinfo.output_height)};
    if (size.x() == 0 || size.y() == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    // JPEG does not support alpha, so all channels are color channels.
    int numColorChannels = cinfo.output_components;
    if (numColorChannels != 1 && numColorChannels != 3) {
        throw ImageLoadError{fmt::format("Unsupported number of color channels: {}", numColorChannels)};
    }

    tlog::debug() << fmt::format("JPEG image info: size={}, numColorChannels={}", size, numColorChannels);

    // Allocate memory for image data
    auto numPixels = static_cast<size_t>(size.x()) * size.y();
    auto numBytesPerPixel = numColorChannels;
    Channel::Data imageData(numPixels * numBytesPerPixel);

    // Create row pointers for libjpeg and then read image
    vector<JSAMPROW> rowPointers(size.y());
    for (int y = 0; y < size.y(); ++y) {
        rowPointers[y] = &imageData[y * size.x() * numBytesPerPixel];
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
    }

    jpeg_finish_decompress(&cinfo);

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    EOrientation orientation = EOrientation::None;

    // Try to extract EXIF data for correct orientation
    if (!exifData.empty()) {
        tlog::debug() << fmt::format("Found EXIF data of size {} bytes", exifData.size());

        try {
            const auto exif = Exif{exifData};
            resultData.attributes.emplace_back(exif.toAttributes());

            const EOrientation exifOrientation = exif.getOrientation();
            if (exifOrientation != EOrientation::None) {
                orientation = exifOrientation;
                tlog::debug() << fmt::format("EXIF image orientation: {}", (int)orientation);
            }

            size = co_await orientToTopLeft(EPixelFormat::U8, imageData, size, orientation, priority);
        } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
    }

    // Same with XMP data. (The +1 is to skip null termination.)
    if (xmpData.size() > xmpNs.size() + 1) {
        const string_view xmpDataView = string_view{(const char*)xmpData.data(), xmpData.size()}.substr(xmpNs.size() + 1);
        tlog::debug() << fmt::format("Found XMP data of size {} bytes", xmpDataView.size());

        try {
            const auto xmp = Xmp{xmpDataView};
            resultData.attributes.emplace_back(xmp.attributes());

            const EOrientation xmpOrientation = xmp.orientation();
            if (xmpOrientation != EOrientation::None) {
                orientation = xmpOrientation;
                tlog::debug() << fmt::format("XMP image orientation: {}", (int)orientation);
            }
        } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read XMP metadata: {}", e.what()); }
    }

    if (orientation != EOrientation::None) {
        size = co_await orientToTopLeft(EPixelFormat::U8, imageData, size, orientation, priority);
    }

    // This JPEG loader is at most 8 bits per channel (technically, JPEG can hold more, but we don't support that here). Thus easily fits
    // into F16.
    resultData.channels = makeRgbaInterleavedChannels(numColorChannels, false, size, EPixelFormat::F32, EPixelFormat::F16);

    // Since JPEG always has no alpha channel, we default to 1, where premultiplied and straight are equivalent.
    resultData.hasPremultipliedAlpha = true;

    // If an ICC profile exists, use it to convert to linear sRGB. Otherwise, assume the decoder gave us sRGB/Rec.709 (per the JPEG spec)
    // and convert it to linear space via inverse sRGB transfer function.
    if (iccProfile) {
        try {
            vector<float> floatData(imageData.size());
            co_await toFloat32(imageData.data(), numColorChannels, floatData.data(), numColorChannels, size, false, priority);

            const auto profile = ColorProfile::fromIcc(iccProfile, iccProfileSize);
            co_await toLinearSrgbPremul(
                profile,
                size,
                numColorChannels,
                EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)floatData.data(),
                resultData.channels.front().floatData(),
                4,
                priority
            );

            resultData.renderingIntent = profile.renderingIntent();
            if (const auto cicp = profile.cicp()) {
                resultData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicp->transfer);
            }

            co_return result;
        } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    co_await toFloat32<uint8_t, true>(imageData.data(), numColorChannels, resultData.channels.front().floatData(), 4, size, false, priority);
    co_return result;
}

} // namespace tev
