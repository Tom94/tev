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
#include <tev/imageio/QoiImageLoader.h>

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include <qoi.h>

using namespace nanogui;
using namespace std;

namespace tev {

bool QoiImageLoader::canLoadFile(istream& iStream) const {
    char b[4];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && string(b, sizeof(b)) == "qoif";

    iStream.clear();
    iStream.seekg(0);
    return result;
}

Task<vector<ImageData>> QoiImageLoader::load(istream& iStream, const fs::path&, const string&, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    char magic[4];
    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (magicString != "qoif") {
        throw invalid_argument{fmt::format("Invalid magic QOI string {}", magicString)};
    }

    iStream.clear();
    iStream.seekg(0, iStream.end);
    size_t dataSize = iStream.tellg();
    iStream.seekg(0, iStream.beg);
    vector<char> data(dataSize);
    iStream.read(data.data(), dataSize);

    qoi_desc desc;
    void* decodedData = qoi_decode(data.data(), static_cast<int>(dataSize), &desc, 0);

    ScopeGuard decodedDataGuard{[decodedData] { free(decodedData); }};

    if (!decodedData) {
        throw invalid_argument{"Failed to decode data from the QOI format."};
    }

    Vector2i size{static_cast<int>(desc.width), static_cast<int>(desc.height)};
    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    int numChannels = static_cast<int>(desc.channels);
    if (numChannels != 4 && numChannels != 3) {
        throw invalid_argument{fmt::format("Invalid number of channels {}.", numChannels)};
    }

    resultData.channels = makeNChannels(numChannels, size);
    resultData.hasPremultipliedAlpha = false;

    if (desc.colorspace == QOI_LINEAR) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                auto typedData = reinterpret_cast<unsigned char*>(decodedData);
                size_t baseIdx = i * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    resultData.channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
                }
            },
            priority
        );
    } else {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                auto typedData = reinterpret_cast<unsigned char*>(decodedData);
                size_t baseIdx = i * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    if (c == 3) {
                        resultData.channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
                    } else {
                        resultData.channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
                    }
                }
            },
            priority
        );
    }

    co_return result;
}

} // namespace tev
