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
#include <tev/imageio/AppleMakerNote.h>
#include <tev/imageio/GainMap.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<void> applyAppleGainMap(float* __restrict image, const float* __restrict gainMap, const Vector2i& size, int priority, const AppleMakerNote* amn) {
    // Apply gain map per https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos

    // 0.0 and 8.0 result in the weakest effect. They are a sane default; see https://developer.apple.com/forums/thread/709331
    float maker33 = 0.0f;
    float maker48 = 8.0f;

    if (amn) {
        maker33 = amn->tryGetFloat(33, maker33);
        maker48 = amn->tryGetFloat(48, maker48);
    }

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

    float headroom = pow(2.0f, max(stops, 0.0f));
    tlog::debug() << fmt::format("Derived gain map headroom {} from maker note entries #33={} and #48={}.", headroom, maker33, maker48);

    const size_t numPixels = (size_t)size.x() * size.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        [&](size_t i) {
            for (int c = 0; c < 3; ++c) {
                image[i * 4 + c] *= (1.0f + (headroom - 1.0f) * gainMap[i * 4]);
            }
        },
        priority
    );

    co_return;
}

} // namespace tev
