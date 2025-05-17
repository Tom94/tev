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

#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/PngImageLoader.h>

#include <png.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> PngImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool) const {
    png_byte header[8] = {0};
    iStream.read(reinterpret_cast<char*>(header), 8);
    if (png_sig_cmp(header, 0, 8)) {
        throw FormatNotSupported{"File is not a PNG image."};
    }

    png_structp pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!pngPtr) {
        throw ImageLoadError{"Failed to create PNG read struct."};
    }

    png_infop infoPtr = nullptr;
    ScopeGuard pngGuard{[&]() { png_destroy_read_struct(&pngPtr, &infoPtr, nullptr); }};

    png_set_error_fn(
        pngPtr,
        nullptr,
        [](png_structp png_ptr, png_const_charp error_msg) { throw ImageLoadError{fmt::format("PNG error: {}", error_msg)}; },
        [](png_structp png_ptr, png_const_charp warning_msg) { tlog::warning() << fmt::format("PNG warning: {}", warning_msg); }
    );

    infoPtr = png_create_info_struct(pngPtr);
    if (!infoPtr) {
        throw ImageLoadError{"Failed to create PNG info struct."};
    }

    png_set_read_fn(pngPtr, &iStream, [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto stream = reinterpret_cast<istream*>(png_get_io_ptr(png_ptr));
        stream->read(reinterpret_cast<char*>(data), length);
        if (stream->gcount() != static_cast<std::streamsize>(length)) {
            png_error(png_ptr, "Read error");
        }
    });

    // Tell libpng we've already read the signature
    png_set_sig_bytes(pngPtr, 8);

    png_read_info(pngPtr, infoPtr);

    png_uint_32 width, height;
    int bitDepth, colorType, interlaceType;
    png_get_IHDR(pngPtr, infoPtr, &width, &height, &bitDepth, &colorType, &interlaceType, nullptr, nullptr);

    Vector2i size{static_cast<int>(width), static_cast<int>(height)};
    if (size.x() == 0 || size.y() == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    // Determine number of channels
    int numColorChannels = 0;
    int numChannels = 0;
    switch (colorType) {
        case PNG_COLOR_TYPE_GRAY: numColorChannels = numChannels = 1; break;
        case PNG_COLOR_TYPE_GRAY_ALPHA:
            numColorChannels = 1;
            numChannels = 2;
            break;
        case PNG_COLOR_TYPE_RGB: numColorChannels = numChannels = 3; break;
        case PNG_COLOR_TYPE_RGB_ALPHA:
            numColorChannels = 3;
            numChannels = 4;
            break;
        case PNG_COLOR_TYPE_PALETTE:
            png_set_palette_to_rgb(pngPtr);
            numColorChannels = numChannels = 3;
            break;
        default: numColorChannels = numChannels = 0; break;
    }

    if (interlaceType != PNG_INTERLACE_NONE) {
        tlog::debug() << "Image is interlaced. Converting to non-interlaced.";
        png_set_interlace_handling(pngPtr);
    }

    // Only grayscale and palette images can have a bit depth of 1, 2, or 4. Since we configure the PNG reader to convert palette images to
    // RGB, we can additionally configure the reader to convert grayscale images with a bit depth of 1, 2, or 4 to 8-bit and then we only
    // have to deal with either 16-bit or 8-bit images.
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        tlog::debug() << fmt::format("Converting grayscale image with bit depth {} to 8-bit.", bitDepth);
        png_set_expand_gray_1_2_4_to_8(pngPtr);
    }

    if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS)) {
        tlog::debug() << "Image has transparency channel. Converting to alpha channel.";

        png_set_tRNS_to_alpha(pngPtr); // Convert transparency to alpha channel
        if (numColorChannels != numChannels) {
            throw ImageLoadError{"Image has transparency channel but already has an alpha channel."};
        }

        numChannels += 1;
    }

    if (numColorChannels == 0 || numChannels == 0) {
        throw ImageLoadError{fmt::format("Unsupported PNG color type: {}", colorType)};
    }

    png_read_update_info(pngPtr, infoPtr);

    bitDepth = png_get_bit_depth(pngPtr, infoPtr);
    if (bitDepth != 8 && bitDepth != 16) {
        throw ImageLoadError{fmt::format("Unsupported PNG bit depth: {}", bitDepth)};
    }

    tlog::debug() << fmt::format(
        "PNG image info: {}x{}, {} channels, {} bit depth, color type: {}", size.x(), size.y(), numChannels, bitDepth, colorType
    );

    // 16 bit channels are big endian by default, but we want little endian on little endian systems
    if (bitDepth == 16 && std::endian::little == std::endian::native) {
        png_set_swap(pngPtr);
    }

    // Allocate memory for image data
    auto numPixels = static_cast<size_t>(size.x()) * size.y();
    auto numBytesPerSample = static_cast<size_t>(bitDepth / 8);
    auto numBytesPerPixel = numBytesPerSample * numChannels;
    vector<png_byte> imageData(numPixels * numBytesPerPixel);

    // Png wants to read into a 2D array of pointers to rows, so we need to create that
    vector<png_bytep> rowPointers(height);
    for (png_uint_32 y = 0; y < height; ++y) {
        rowPointers[y] = &imageData[y * width * numBytesPerPixel];
    }

    png_read_image(pngPtr, rowPointers.data());

    optional<AttributeNode> exifAttributes;

    png_uint_32 exifDataSize = 0;
    png_bytep exifDataRaw = nullptr;
    png_get_eXIf_1(pngPtr, infoPtr, &exifDataSize, &exifDataRaw);
    if (exifDataRaw) {
        // libpng strips the exif header, but our exif library actually wants the header, so we prepend it again.
        std::vector<uint8_t> exifData(exifDataRaw, exifDataRaw + exifDataSize);
        Exif::prependFourcc(&exifData);

        tlog::debug() << fmt::format("Found EXIF data of size {} bytes", exifData.size());

        try {
            const auto exif = Exif(exifData);
            exifAttributes = exif.toAttributes();

            EOrientation orientation = exif.getOrientation();
            tlog::debug() << fmt::format("EXIF image orientation: {}", (int)orientation);

            co_await orientToTopLeft(imageData, size, orientation, priority);
        } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
    }

    bool hasAlpha = numChannels > numColorChannels;

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    if (exifAttributes) {
        resultData.attributes.emplace_back(exifAttributes.value());
    }

    resultData.channels = makeRgbaInterleavedChannels(numChannels, hasAlpha, size);
    resultData.hasPremultipliedAlpha = false;

    // According to http://www.libpng.org/pub/png/spec/1.2/PNG-Contents.html, if an ICC profile exists, use it to convert to linear sRGB.
    // Otherwise, check the sRGB chunk for whether the image is in sRGB/Rec709. If not, then check the gAMA and cHRM chunks for gamma and
    // chromaticity values and use them to convert to linear sRGB. And lastly (this isn't in the spec, but based on having seen a lot of
    // PNGs), if none of these chunks are present, fall back to sRGB.
    png_bytep iccProfile = nullptr;
    png_charp iccProfileName = nullptr;
    png_uint_32 iccProfileSize = 0;
    if (png_get_iCCP(pngPtr, infoPtr, &iccProfileName, nullptr, &iccProfile, &iccProfileSize) == PNG_INFO_iCCP) {
        tlog::debug() << fmt::format("Found ICC color profile: {}, attempting to apply...", iccProfileName);

        try {
            vector<float> floatData(imageData.size());
            if (bitDepth == 16) {
                co_await toFloat32((uint16_t*)imageData.data(), numChannels, floatData.data(), numChannels, size, hasAlpha, priority);
            } else {
                co_await toFloat32(imageData.data(), numChannels, floatData.data(), numChannels, size, hasAlpha, priority);
            }

            co_await toLinearSrgbPremul(
                ColorProfile::fromIcc(iccProfile, iccProfileSize),
                size,
                numColorChannels,
                numChannels > numColorChannels ? EAlphaKind::Straight : EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)floatData.data(),
                resultData.channels.front().data(),
                priority
            );

            resultData.hasPremultipliedAlpha = true;
            co_return result;
        } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    int srgbIntent = 0;
    bool hasChunkSrgb = png_get_sRGB(pngPtr, infoPtr, &srgbIntent) == PNG_INFO_iCCP;

    double gamma64 = 2.2;
    bool hasChunkGama = png_get_gAMA(pngPtr, infoPtr, &gamma64) == PNG_INFO_gAMA;
    float gamma = (float)gamma64;

    array<double, 8> ch = {0};
    bool hasChunkChrm = png_get_cHRM(pngPtr, infoPtr, &ch[0], &ch[1], &ch[2], &ch[3], &ch[4], &ch[5], &ch[6], &ch[7]) == PNG_INFO_cHRM;

    if (hasChunkSrgb || (!hasChunkGama && !hasChunkChrm)) {
        if (hasChunkSrgb) {
            tlog::debug() << fmt::format("Using sRGB chunk w/ rendering intent {}", srgbIntent);
        } else {
            tlog::debug() << "No iCCP, sRGB, gAMA, or cHRM chunks found. Using sRGB by default.";
        }

        if (bitDepth == 16) {
            co_await toFloat32<uint16_t, true>(
                (uint16_t*)imageData.data(), numChannels, resultData.channels.front().data(), 4, size, hasAlpha, priority
            );
        } else {
            co_await toFloat32<uint8_t, true>(imageData.data(), numChannels, resultData.channels.front().data(), 4, size, hasAlpha, priority);
        }

        co_return result;
    }

    tlog::debug() << fmt::format("Using gamma={}", gamma64);
    if (bitDepth == 16) {
        co_await toFloat32<uint16_t, false>(
            (uint16_t*)imageData.data(), numChannels, resultData.channels.front().data(), 4, size, hasAlpha, priority
        );
    } else {
        co_await toFloat32<uint8_t, false>(imageData.data(), numChannels, resultData.channels.front().data(), 4, size, hasAlpha, priority);
    }

    auto* pixelData = resultData.channels.front().data();
    co_await ThreadPool::global().parallelForAsync<float>(
        0,
        numPixels,
        [&](size_t i) {
            for (int c = 0; c < 3; ++c) {
                pixelData[i * 4 + c] = pow(pixelData[i * 4 + c], gamma);
            }
        },
        priority
    );

    if (hasChunkChrm) {
        tlog::debug() << fmt::format(
            "Using cHRM chunk with chromaticity values: R({:.4f}, {:.4f}), G({:.4f}, {:.4f}), B({:.4f}, {:.4f}), W({:.4f}, {:.4f})",
            ch[2],
            ch[3],
            ch[4],
            ch[5],
            ch[6],
            ch[7],
            ch[0],
            ch[1]
        );

        resultData.toRec709 = chromaToRec709Matrix({
            {
             {(float)ch[2], (float)ch[3]}, // red
                {(float)ch[4], (float)ch[5]}, // green
                {(float)ch[6], (float)ch[7]}, // blue
                {(float)ch[0], (float)ch[1]}, // white
            }
        });
    }

    co_return result;
}

} // namespace tev
