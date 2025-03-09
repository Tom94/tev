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
#include <tev/imageio/PfmImageLoader.h>

#include <bit>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> PfmImageLoader::load(istream& iStream, const fs::path&, const string&, int priority, bool) const {
    string magic;
    Vector2i size;
    float scale;

    iStream >> magic >> size.x() >> size.y() >> scale;

    int numChannels;
    if (magic == "Pf") {
        numChannels = 1;
    } else if (magic == "PF") {
        numChannels = 3;
    } else if (magic == "PF4") {
        numChannels = 4;
    } else {
        throw FormatNotSupportedException{fmt::format("Invalid magic PFM string {}", magic)};
    }

    if (!isfinite(scale) || scale == 0) {
        throw invalid_argument{fmt::format("Invalid PFM scale {}", scale)};
    }

    bool isPfmLittleEndian = scale < 0;
    scale = abs(scale);

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    resultData.channels = makeNChannels(numChannels, size);

    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    auto numFloats = numPixels * numChannels;
    auto numBytes = numFloats * sizeof(float);

    // Skip last newline at the end of the header.
    {
        char c;
        while (iStream.get(c) && c != '\r' && c != '\n')
            ;
    }

    // Read entire file in binary mode.
    vector<float> data(numFloats);
    iStream.read(reinterpret_cast<char*>(data.data()), numBytes);
    if (iStream.gcount() < (streamsize)numBytes) {
        throw invalid_argument{fmt::format("Not sufficient bytes to read ({} vs {})", iStream.gcount(), numBytes)};
    }

    // Reverse bytes of every float if endianness does not match up with system
    const bool shallSwapBytes = (std::endian::native == std::endian::little) != isPfmLittleEndian;

    co_await ThreadPool::global().parallelForAsync(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                int baseIdx = (y * size.x() + x) * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    float val = data[baseIdx + c];

                    // Thankfully, due to branch prediction, the "if" in the inner loop is no significant overhead.
                    if (shallSwapBytes) {
                        val = swapBytes(val);
                    }

                    // Flip image vertically due to PFM format
                    resultData.channels[c].at({x, size.y() - (int)y - 1}) = scale * val;
                }
            }
        },
        priority
    );

    resultData.hasPremultipliedAlpha = false;

    co_return result;
}

} // namespace tev
