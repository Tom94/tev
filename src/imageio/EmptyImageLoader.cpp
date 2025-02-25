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

bool EmptyImageLoader::canLoadFile(istream& iStream) const {
    char b[5];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && string(b, sizeof(b)) == "empty";

    iStream.clear();
    iStream.seekg(0);
    return result;
}

Task<vector<ImageData>> EmptyImageLoader::load(istream& iStream, const fs::path&, const string&, int) const {
    vector<ImageData> result(1);
    ImageData& data = result.front();

    string magic;
    Vector2i size;
    int nChannels;
    iStream >> magic >> size.x() >> size.y() >> nChannels;

    if (magic != "empty") {
        throw invalid_argument{fmt::format("Invalid magic empty string {}", magic)};
    }

    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

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
