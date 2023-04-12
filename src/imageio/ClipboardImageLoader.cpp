// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ClipboardImageLoader.h>
#include <tev/ThreadPool.h>

#include <clip.h>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

bool ClipboardImageLoader::canLoadFile(istream& iStream) const {
    char b[4];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && string(b, sizeof(b)) == "clip";

    iStream.clear();
    iStream.seekg(0);
    return result;
}

Task<vector<ImageData>> ClipboardImageLoader::load(istream& iStream, const fs::path&, const string& channelSelector, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    char magic[4];
    clip::image_spec spec;

    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (magicString != "clip") {
        throw invalid_argument{format("Invalid magic clipboard string {}", magicString)};
    }

    iStream.read(reinterpret_cast<char*>(&spec), sizeof(clip::image_spec));
    if (iStream.gcount() < (streamsize)sizeof(clip::image_spec)) {
        throw invalid_argument{format("Not sufficient bytes to read image spec ({} vs {})", iStream.gcount(), sizeof(clip::image_spec))};
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
        throw invalid_argument{format("Not sufficient bytes to read image data ({} vs {})", iStream.gcount(), numBytes)};
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
    bool premultipliedAlpha = false && numChannels >= 4;
    co_await ThreadPool::global().parallelForAsync(0, size.y(), [&](int y) {
        for (int x = 0; x < size.x(); ++x) {
            int baseIdx = y * numBytesPerRow + x * numChannels;
            for (int c = numChannels-1; c >= 0; --c) {
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
    }, priority);

    resultData.hasPremultipliedAlpha = false;

    co_return result;
}

TEV_NAMESPACE_END
