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

#include <tev/imageio/EmptyImageLoader.h>

#include <istream>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> EmptyImageLoader::load(istream& iStream, const fs::path&, const string&, int, bool) const {
    char magic[6];
    iStream.read(magic, 6);
    string magicString(magic, 6);

    if (!iStream || magicString != "empty ") {
        throw FormatNotSupported{fmt::format("Invalid magic empty string {}.", magic)};
    }

    Vector2i size;
    int nChannels;
    iStream >> size.x() >> size.y() >> nChannels;

    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw LoadError{"Image has zero pixels."};
    }

    vector<ImageData> result(1);
    ImageData& data = result.front();

    for (int i = 0; i < nChannels; ++i) {
        // The following lines decode strings by prefix length. The reason for using sthis encoding is to allow arbitrary characters,
        // including whitespaces, in the channel names.
        std::vector<char> channelNameData;
        int length;
        iStream >> length;
        channelNameData.resize(length + 1, 0);
        iStream.read(channelNameData.data(), length);

        string channelName = channelNameData.data();

        data.channels.emplace_back(Channel{channelName, size});
        data.channels.back().setZero();
    }

    data.hasPremultipliedAlpha = true;

    co_return result;
}

} // namespace tev
