// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/PfmImageLoader.h>
#include <tev/ThreadPool.h>

using namespace filesystem;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

bool PfmImageLoader::canLoadFile(istream& iStream) const {
    char b[2];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && b[0] == 'P' && (b[1] == 'F' || b[1] == 'f');

    iStream.clear();
    iStream.seekg(0);
    return result;
}

Task<std::tuple<ImageData, bool>> PfmImageLoader::load(istream& iStream, const path&, const string& channelSelector, int priority) const {
    ImageData result;

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
        throw invalid_argument{tfm::format("Invalid magic PFM string %s", magic)};
    }

    if (!isfinite(scale) || scale == 0) {
        throw invalid_argument{tfm::format("Invalid PFM scale %f", scale)};
    }

    bool isPfmLittleEndian = scale < 0;
    scale = abs(scale);

    vector<Channel> channels = makeNChannels(numChannels, size);

    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    auto numFloats = numPixels * numChannels;
    auto numBytes = numFloats * sizeof(float);

    // Skip last newline at the end of the header.
    char c;
    while (iStream.get(c) && c != '\r' && c != '\n');

    // Read entire file in binary mode.
    vector<float> data(numFloats);
    iStream.read(reinterpret_cast<char*>(data.data()), numBytes);
    if (iStream.gcount() < (streamsize)numBytes) {
        throw invalid_argument{tfm::format("Not sufficient bytes to read (%d vs %d)", iStream.gcount(), numBytes)};
    }

    // Reverse bytes of every float if endianness does not match up with system
    const bool shallSwapBytes = isSystemLittleEndian() != isPfmLittleEndian;

    co_await gThreadPool->parallelForAsync(0, size.y(), [&](int y) {
        for (int x = 0; x < size.x(); ++x) {
            int baseIdx = (y * size.x() + x) * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                float val = data[baseIdx + c];

                // Thankfully, due to branch prediction, the "if" in the
                // inner loop is no significant overhead.
                if (shallSwapBytes) {
                    val = swapBytes(val);
                }

                // Flip image vertically due to PFM format
                channels[c].at({x, size.y() - (int)y - 1}) = scale * val;
            }
        }
    }, priority);

    vector<pair<size_t, size_t>> matches;
    for (size_t i = 0; i < channels.size(); ++i) {
        size_t matchId;
        if (matchesFuzzy(channels[i].name(), channelSelector, &matchId)) {
            matches.emplace_back(matchId, i);
        }
    }

    if (!channelSelector.empty()) {
        sort(begin(matches), end(matches));
    }

    for (const auto& match : matches) {
        result.channels.emplace_back(move(channels[match.second]));
    }

    // PFM can not contain layers, so all channels simply reside
    // within a topmost root layer.
    result.layers.emplace_back("");

    // PFM images do not have custom data and display windows.
    result.dataWindow = result.displayWindow = result.channels.front().size();

    co_return {result, false};
}

TEV_NAMESPACE_END
