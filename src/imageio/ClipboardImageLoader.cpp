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
#include <tev/imageio/ClipboardImageLoader.h>

#include <clip.h>

using namespace nanogui;
using namespace std;

namespace tev {

bool ClipboardImageLoader::canLoadFile(istream& iStream) const {
    char b[4];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && string(b, sizeof(b)) == "clip";

    iStream.clear();
    iStream.seekg(0);
    return result;
}

Task<vector<ImageData>> ClipboardImageLoader::load(istream& iStream, const fs::path&, const string&, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    char magic[4];
    clip::image_spec spec;

    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (magicString != "clip") {
        throw invalid_argument{fmt::format("Invalid magic clipboard string {}", magicString)};
    }

    iStream.read(reinterpret_cast<char*>(&spec), sizeof(clip::image_spec));
    if (iStream.gcount() < (streamsize)sizeof(clip::image_spec)) {
        throw invalid_argument{fmt::format("Not sufficient bytes to read image spec ({} vs {})", iStream.gcount(), sizeof(clip::image_spec))};
    }

    Vector2i size{(int)spec.width, (int)spec.height};

    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    auto numChannels = (int)(spec.bits_per_pixel / 8);
    if (numChannels > 4) {
        throw invalid_argument{"Image has too many channels."};
    }

    auto numBytesPerRow = numChannels * size.x();
    auto numBytes = (size_t)numBytesPerRow * size.y();
    int alphaChannelIndex = 3;

    resultData.channels = makeNChannels(numChannels, size);

    vector<char> data(numBytes);
    iStream.read(reinterpret_cast<char*>(data.data()), numBytes);
    if (iStream.gcount() < (streamsize)numBytes) {
        throw invalid_argument{fmt::format("Not sufficient bytes to read image data ({} vs {})", iStream.gcount(), numBytes)};
    }

    int shifts[4] = {
        (int)(spec.red_shift / 8),
        (int)(spec.green_shift / 8),
        (int)(spec.blue_shift / 8),
        (int)(spec.alpha_shift / 8),
    };

    for (int c = 0; c < numChannels; ++c) {
        if (shifts[c] >= numChannels) {
            throw invalid_argument{"Invalid shift encountered in clipboard image."};
        }
    }

    // TODO: figure out when alpha is already premultiplied (prior to tonemapping).
    //       clip doesn't properly handle this... so copy&pasting transparent images
    //       from browsers tends to produce incorrect color values in alpha!=1/0 regions.
    bool premultipliedAlpha = false;// && numChannels >= 4;
    co_await ThreadPool::global().parallelForAsync(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                int baseIdx = y * numBytesPerRow + x * numChannels;
                for (int c = numChannels - 1; c >= 0; --c) {
                    unsigned char val = data[baseIdx + shifts[c]];
                    if (c == alphaChannelIndex) {
                        resultData.channels[c].at({x, y}) = val / 255.0f;
                    } else {
                        float alpha = premultipliedAlpha ? resultData.channels[alphaChannelIndex].at({x, y}) : 1.0f;
                        float alphaFactor = alpha == 0 ? 0 : (1.0f / alpha);
                        resultData.channels[c].at({x, y}) = toLinear(val / 255.0f * alphaFactor);
                    }
                }
            }
        },
        priority
    );

    resultData.hasPremultipliedAlpha = false;

    co_return result;
}

} // namespace tev
