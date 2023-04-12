// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/PfmImageLoader.h>
#include <tev/ThreadPool.h>

#include <bit>

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

Task<vector<ImageData>> PfmImageLoader::load(istream& iStream, const fs::path&, const string& channelSelector, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

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
        throw invalid_argument{format("Invalid magic PFM string {}", magic)};
    }

    if (!isfinite(scale) || scale == 0) {
        throw invalid_argument{format("Invalid PFM scale {}", scale)};
    }

    bool isPfmLittleEndian = scale < 0;
    scale = abs(scale);

    resultData.channels = makeNChannels(numChannels, size);

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
        throw invalid_argument{format("Not sufficient bytes to read ({} vs {})", iStream.gcount(), numBytes)};
    }

    // Reverse bytes of every float if endianness does not match up with system
    const bool shallSwapBytes = (std::endian::native == std::endian::little) != isPfmLittleEndian;

    co_await ThreadPool::global().parallelForAsync(0, size.y(), [&](int y) {
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
                resultData.channels[c].at({x, size.y() - (int)y - 1}) = scale * val;
            }
        }
    }, priority);

    resultData.hasPremultipliedAlpha = false;

    co_return result;
}

TEV_NAMESPACE_END
