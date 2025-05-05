/*
 * tev -- the EXR viewer
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

#include <jpeglib.h>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {
// Taken from jpegdecoderhelper in libultrahdr per their Apache 2.0 license and shortened.
// https://github.com/google/libultrahdr/blob/6db3a83ee2b1f79850f3f597172289808dc6a331/lib/src/jpegdecoderhelper.cpp#L125
void jpeg_extract_marker_payload(
    const j_decompress_ptr cinfo,
    const uint32_t marker_code,
    const uint8_t* marker_fourcc_code,
    const uint32_t fourcc_length,
    std::vector<JOCTET>& destination
) {
    for (jpeg_marker_struct* marker = cinfo->marker_list; marker; marker = marker->next) {
        if (marker->marker == marker_code && marker->data_length > fourcc_length && !memcmp(marker->data, marker_fourcc_code, fourcc_length)) {
            destination.resize(marker->data_length);
            memcpy(static_cast<void*>(destination.data()), marker->data, marker->data_length);
            return;
        }
    }
}
} // namespace

Task<vector<ImageData>> JpegTurboImageLoader::load(istream& iStream, const fs::path&, const string&, int priority, bool) const {
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
        throw LoadError{fmt::format("JPEG error: {}", buffer)};
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
        throw LoadError{"Failed to read JPEG header."};
    }

    static const uint8_t EXIF_FOURCC[] = {
        'E',
        'x',
        'i',
        'f',
        '\0',
        '\0',
    };

    std::vector<JOCTET> exifData;
    jpeg_extract_marker_payload(&cinfo, JPEG_APP0 + 1, EXIF_FOURCC, sizeof(EXIF_FOURCC), exifData);

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
        throw LoadError{"Image has zero pixels."};
    }

    // JPEG does not support alpha, so all channels are color channels.
    int numColorChannels = cinfo.output_components;
    if (numColorChannels != 1 && numColorChannels != 3) {
        throw LoadError{fmt::format("Unsupported number of color channels: {}", numColorChannels)};
    }

    tlog::debug() << fmt::format("JPEG image info: {}x{}, {} channels", size.x(), size.y(), numColorChannels);

    // Allocate memory for image data
    auto numPixels = static_cast<size_t>(size.x()) * size.y();
    auto numBytesPerPixel = numColorChannels;
    vector<uint8_t> imageData(numPixels * numBytesPerPixel);

    // Create row pointers for libjpeg and then read image
    vector<JSAMPROW> rowPointers(size.y());
    for (int y = 0; y < size.y(); ++y) {
        rowPointers[y] = &imageData[y * size.x() * numBytesPerPixel];
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &rowPointers[cinfo.output_scanline], cinfo.output_height - cinfo.output_scanline);
    }

    jpeg_finish_decompress(&cinfo);

    // Try to extract EXIF data for correct orientation
    if (!exifData.empty()) {
        tlog::debug() << fmt::format("Found EXIF data of size {} bytes", exifData.size());

        try {
            EOrientation orientation = Exif(exifData).getOrientation();
            tlog::debug() << fmt::format("EXIF image orientation: {}", (int)orientation);

            co_await orientToTopLeft(imageData, size, orientation, priority);
        } catch (const invalid_argument& e) {
            tlog::warning() << fmt::format("Failed reorient from EXIF: {}", e.what());
        }
    }

    vector<ImageData> result(1);
    ImageData& resultData = result.front();
    resultData.channels = makeNChannels(numColorChannels, size);

    // Since JPEG always has no alpha channel, we default to 1, where premultiplied and straight are equivalent.
    resultData.hasPremultipliedAlpha = true;

    // If an ICC profile exists, use it to convert to linear sRGB. Otherwise, assume the decoder gave us sRGB/Rec.709 (per the JPEG spec)
    // and convert it to linear space via inverse sRGB transfer function.
    if (iccProfile) {
        try {
            vector<float> floatData(imageData.size());
            co_await toFloat32(imageData.data(), numColorChannels, floatData.data(), numColorChannels, size, false, priority);
            co_await toLinearSrgbPremul(
                ColorProfile::fromIcc(iccProfile, iccProfileSize),
                size,
                numColorChannels,
                EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)floatData.data(),
                resultData.channels.front().data(),
                priority
            );

            co_return result;
        } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    co_await toFloat32<uint8_t, true>(imageData.data(), numColorChannels, resultData.channels.front().data(), 4, size, false, priority);
    co_return result;
}

} // namespace tev
