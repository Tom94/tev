// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/QoiImageLoader.h>
#include <tev/ThreadPool.h>

#include <qoi.h>

using namespace filesystem;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

bool QoiImageLoader::canLoadFile(istream& iStream) const {
    char b[4];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && string(b, sizeof(b)) == "qoif";

    iStream.clear();
    iStream.seekg(0);
    return result;
}

Task<vector<ImageData>> QoiImageLoader::load(istream& iStream, const path&, const string& channelSelector, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    char magic[4];
    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (magicString != "qoif") {
        throw invalid_argument{tfm::format("Invalid magic QOI string %s", magicString)};
    }

    iStream.clear();
    iStream.seekg(0, iStream.end);
    size_t dataSize = iStream.tellg();
    iStream.seekg(0, iStream.beg);
    vector<char> data(dataSize);
    iStream.read(data.data(), dataSize);

    // FIXME: we assume numChannels = 4 as this information is not stored in the format.
    // TODO: Update this when the final QOI data format is decided.
    // https://github.com/phoboslab/qoi/issues/37
    int numChannels = 4;
    Vector2i size;

    void* decodedData = qoi_decode(data.data(), static_cast<int>(dataSize), &size.x(), &size.y(), numChannels);

    ScopeGuard decodedDataGuard{[decodedData] { free(decodedData); }};

    if (!decodedData) {
        throw invalid_argument{"Failed to decode data from the QOI format."};
    }

    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    resultData.channels = makeNChannels(numChannels, size);
    int alphaChannelIndex = 3;

    co_await gThreadPool->parallelForAsync<size_t>(0, numPixels, [&](size_t i) {
        auto typedData = reinterpret_cast<unsigned char*>(decodedData);
        size_t baseIdx = i * numChannels;
        for (int c = 0; c < numChannels; ++c) {
            // TODO: Update this when the final QOI data format is decided.
            // https://github.com/phoboslab/qoi/issues/37
            if (c == alphaChannelIndex) {
                resultData.channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
            } else {
                resultData.channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
            }
        }
    }, priority);

    // TODO: Update this when the final QOI data format is decided.
    // https://github.com/phoboslab/qoi/issues/37
    resultData.hasPremultipliedAlpha = false;

    co_return result;
}

TEV_NAMESPACE_END
