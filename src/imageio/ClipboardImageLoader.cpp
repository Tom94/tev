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
#include <tev/imageio/ClipboardImageLoader.h>

#include <clip.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> ClipboardImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    char magic[4];
    clip::image_spec spec;

    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (!iStream || magicString != "clip") {
        throw FormatNotSupported{fmt::format("Invalid magic clipboard string {}.", magicString)};
    }

    iStream.read(reinterpret_cast<char*>(&spec), sizeof(clip::image_spec));
    if (iStream.gcount() < (streamsize)sizeof(clip::image_spec)) {
        throw ImageLoadError{fmt::format("Insufficient bytes to read image spec ({} vs {}).", iStream.gcount(), sizeof(clip::image_spec))};
    }

    const Vector2i size{(int)spec.width, (int)spec.height};

    const auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    const auto numChannels = (size_t)(spec.bits_per_pixel / 8);
    if (numChannels > 4) {
        throw ImageLoadError{"Image has too many channels."};
    }

    const size_t numBytesPerRow = spec.bytes_per_row;
    const size_t numBytes = numBytesPerRow * size.y();
    const size_t alphaChannelIndex = 3;

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    // Clipboard images are always 32 bit RGBA. Can be comfortably represented as F16.
    const auto numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
    resultData.channels = co_await makeRgbaInterleavedChannels(
        numChannels, numInterleavedChannels, numChannels == 4, size, EPixelFormat::F32, EPixelFormat::F16, "", priority
    );

    HeapArray<char> data(numBytes);
    iStream.read(reinterpret_cast<char*>(data.data()), numBytes);
    if (iStream.gcount() < (streamsize)numBytes) {
        throw ImageLoadError{fmt::format("Insufficient bytes to read image data ({} vs {}).", iStream.gcount(), numBytes)};
    }

    const size_t shifts[4] = {
        (size_t)(spec.red_shift / 8),
        (size_t)(spec.green_shift / 8),
        (size_t)(spec.blue_shift / 8),
        (size_t)(spec.alpha_shift / 8),
    };

    for (size_t c = 0; c < numChannels; ++c) {
        if (shifts[c] >= numChannels) {
            throw ImageLoadError{"Invalid shift encountered in clipboard image."};
        }
    }

    float* const floatData = resultData.channels.front().floatData();
    co_await ThreadPool::global().parallelForAsync(
        0,
        size.y(),
        numPixels * numChannels,
        [&](int y) {
            const size_t rowIdxIn = y * numBytesPerRow;
            const size_t rowIdxOut = y * size.x() * numInterleavedChannels;

            for (int x = 0; x < size.x(); ++x) {
                float alpha = 1.0f;

                const size_t baseIdxIn = rowIdxIn + x * numChannels;
                const size_t baseIdxOut = rowIdxOut + x * numInterleavedChannels;

                for (int c = (int)numChannels - 1; c >= 0; ++c) {
                    const unsigned char val = data[baseIdxIn + shifts[c]];
                    if (c == alphaChannelIndex) {
                        alpha = val / 255.0f;
                        floatData[baseIdxOut + numInterleavedChannels - 1] = alpha;
                    } else {
                        floatData[baseIdxOut + c] = toLinear(val / 255.0f) * alpha;
                    }
                }
            }
        },
        priority
    );

    resultData.hasPremultipliedAlpha = true;

    co_return result;
}

} // namespace tev
