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
#include <tev/imageio/AppleMakerNote.h>
#include <tev/imageio/GainMap.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<void> applyAppleGainMap(ImageData& image, const ImageData& gainMap, int priority, const AppleMakerNote& amn) {
    auto size = image.channels[0].size();
    TEV_ASSERT(size == gainMap.channels[0].size(), "Image and gain map must have the same size");

    // Apply gain map per https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos
    float headroom = 1.0f;
    try {
        // 0.0 and 8.0 result in the weakest effect. They are a sane default; see https://developer.apple.com/forums/thread/709331
        float maker33 = amn.tryGetFloat<float>(33, 0.0f);
        float maker48 = amn.tryGetFloat<float>(48, 8.0f);

        tlog::debug() << fmt::format("Maker 33: {} Maker 48: {}", maker33, maker48);

        float stops;
        if (maker33 < 1.0f) {
            if (maker48 <= 0.01f) {
                stops = -20.0f * maker48 + 1.8f;
            } else {
                stops = -0.101f * maker48 + 1.601f;
            }
        } else {
            if (maker48 <= 0.01f) {
                stops = -70.0f * maker48 + 3.0f;
            } else {
                stops = -0.303f * maker48 + 2.303f;
            }
        }

        headroom = pow(2.0f, max(stops, 0.0f));
        tlog::debug() << fmt::format("Found apple gain map headroom: {}", headroom);
    } catch (const std::invalid_argument& e) {
        tlog::warning() << fmt::format("Failed to read gain map headroom: {}", e.what());
        co_return;
    }

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                size_t i = y * (size_t)size.x() + x;
                for (int c = 0; c < 3; ++c) {
                    image.channels[c].at(i) *= (1.0f + (headroom - 1.0f) * gainMap.channels[0].at(i));
                }
            }
        },
        priority
    );

    co_return;
}

} // namespace tev
