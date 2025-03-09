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
#include <tev/imageio/StbiImageLoader.h>

#include <stb_image.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> StbiImageLoader::load(istream& iStream, const fs::path&, const string&, int priority, bool) const {
    static const stbi_io_callbacks callbacks = {
        // Read
        [](void* context, char* data, int size) {
            auto stream = reinterpret_cast<istream*>(context);
            stream->read(data, size);
            return (int)stream->gcount();
        },
        // Seek
        [](void* context, int size) { reinterpret_cast<istream*>(context)->seekg(size, ios_base::cur); },
        // EOF
        [](void* context) { return (int)!!(*reinterpret_cast<istream*>(context)); },
    };

    void* data;
    int numChannels;
    Vector2i size;
    bool isHdr = stbi_is_hdr_from_callbacks(&callbacks, &iStream) != 0;
    iStream.clear();
    iStream.seekg(0);

    if (isHdr) {
        data = stbi_loadf_from_callbacks(&callbacks, &iStream, &size.x(), &size.y(), &numChannels, 0);
    } else {
        data = stbi_load_from_callbacks(&callbacks, &iStream, &size.x(), &size.y(), &numChannels, 0);
    }

    if (!data) {
        throw invalid_argument{std::string{stbi_failure_reason()}};
    }

    if (size.x() == 0 || size.y() == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    ScopeGuard dataGuard{[data] { stbi_image_free(data); }};

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    resultData.channels = makeNChannels(numChannels, size);
    static const int ALPHA_CHANNEL_INDEX = 3;

    auto numPixels = (size_t)size.x() * size.y();
    if (isHdr) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                auto typedData = reinterpret_cast<float*>(data);
                size_t baseIdx = i * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    resultData.channels[c].at(i) = typedData[baseIdx + c];
                }
            },
            priority
        );
    } else {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                auto typedData = reinterpret_cast<unsigned char*>(data);
                size_t baseIdx = i * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    if (c == ALPHA_CHANNEL_INDEX) {
                        resultData.channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
                    } else {
                        resultData.channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
                    }
                }
            },
            priority
        );
    }

    resultData.hasPremultipliedAlpha = false;

    co_return result;
}

} // namespace tev
