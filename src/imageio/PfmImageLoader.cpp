// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/PfmImageLoader.h>
#include <tev/ThreadPool.h>

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

bool PfmImageLoader::canLoadFile(ifstream& f) const {
    char b[2];
    f.read(b, sizeof(b));

    bool result = !!f && f.gcount() == sizeof(b) && b[0] == 'P' && (b[1] == 'F' || b[1] == 'f');

    f.clear();
    f.seekg(0);
    return result;
}

ImageData PfmImageLoader::load(ifstream& f, const filesystem::path&, const string& channelSelector) const {
    ImageData result;
    ThreadPool threadPool;

    string magic;
    Vector2i size;
    float scale;

    f >> magic >> size.x() >> size.y() >> scale;

    if (magic != "PF" && magic != "Pf") {
        throw invalid_argument{tfm::format("Invalid magic PFM string %s", magic)};
    }

    if (!isfinite(scale) || scale == 0) {
        throw invalid_argument{tfm::format("Invalid PFM scale %f", scale)};
    }

    int numChannels = magic[1] == 'F' ? 3 : 1;
    bool isPfmLittleEndian = scale < 0;
    scale = abs(scale);

    vector<Channel> channels = makeNChannels(numChannels, size);

    auto numPixels = (DenseIndex)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }
    
    auto numFloats = numPixels * numChannels;
    auto numBytes = numFloats * sizeof(float);

    // Skip last newline at the end of the header.
    string line;
    getline(f, line);

    // Read entire file in binary mode.
    vector<float> data(numFloats);
    f.read(reinterpret_cast<char*>(data.data()), numBytes);
    if (f.gcount() < (streamsize)numBytes) {
        throw invalid_argument{tfm::format("Not sufficient bytes to read (%d vs %d)", f.gcount(), numBytes)};
    }

    // Reverse bytes of every float if endianness does not match up with system
    const bool shallSwapBytes = isSystemLittleEndian() != isPfmLittleEndian;

    threadPool.parallelFor<DenseIndex>(0, size.y(), [&](DenseIndex y) {
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
                channels[c].at({x, size.y() - y - 1}) = scale * val;
            }
        }
    });

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

    return result;
}

TEV_NAMESPACE_END
