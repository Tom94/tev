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

#include <tev/ThreadPool.h>
#include <tev/imageio/StbiImageLoader.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> StbiImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool) const {
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
    int numFrames = 1;
    Vector2i size;
    bool isHdr = stbi_is_hdr_from_callbacks(&callbacks, &iStream) != 0;
    iStream.clear();
    iStream.seekg(0);

    if (isHdr) {
        data = stbi_loadf_from_callbacks(&callbacks, &iStream, &size.x(), &size.y(), &numChannels, 0);
    } else {
        stbi__context s;
        stbi__start_callbacks(&s, (stbi_io_callbacks*)&callbacks, &iStream);
        bool isGif = stbi__gif_test(&s);
        iStream.clear();
        iStream.seekg(0);

        if (isGif) {
            stbi__start_callbacks(&s, (stbi_io_callbacks*)&callbacks, &iStream);
            int* delays; // We don't care about gif frame delays. Read and discard.
            data = stbi__load_gif_main(&s, &delays, &size.x(), &size.y(), &numFrames, &numChannels, 0);
        } else {
            data = stbi_load_from_callbacks(&callbacks, &iStream, &size.x(), &size.y(), &numChannels, 0);
        }
    }

    if (!data) {
        throw ImageLoadError{std::string{stbi_failure_reason()}};
    }

    if (numFrames == 0) {
        throw ImageLoadError{"Image has zero frames."};
    }

    if (size.x() == 0 || size.y() == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    ScopeGuard dataGuard{[data] { stbi_image_free(data); }};

    vector<ImageData> result(numFrames);
    for (int frameIdx = 0; frameIdx < numFrames; ++frameIdx) {
        ImageData& resultData = result[frameIdx];
        if (numFrames > 1) {
            resultData.partName = fmt::format("frames.{}", frameIdx);
        }

        // Unless the image is a .hdr file, it's 8 bits per channel, so we can comfortably fit it into F16.
        resultData.channels = co_await makeRgbaInterleavedChannels(
            numChannels, numChannels == 4, size, EPixelFormat::F32, isHdr ? EPixelFormat::F32 : EPixelFormat::F16, resultData.partName, priority
        );
        resultData.hasPremultipliedAlpha = false;

        const auto numPixels = (size_t)size.x() * size.y();
        if (isHdr) {
            // Treated like EXR: scene-referred by nature. Usually corresponds to linear light, so should not get its white point adjusted.
            resultData.renderingIntent = ERenderingIntent::AbsoluteColorimetric;

            co_await toFloat32((float*)data, numChannels, resultData.channels.front().floatData(), 4, size, numChannels == 4, priority);
            data = (float*)data + numPixels * numChannels;
        } else {
            // Assume sRGB-encoded LDR images are display-referred.
            resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;

            co_await toFloat32<uint8_t, true>(
                (uint8_t*)data, numChannels, resultData.channels.front().floatData(), 4, size, numChannels == 4, priority
            );
            data = (uint8_t*)data + numPixels * numChannels;
        }
    }

    co_return result;
}

} // namespace tev
