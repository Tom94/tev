// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/QoiImageLoader.h>
#include <tev/ThreadPool.h>

#define QOI_NO_STDIO
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

    qoi_desc desc;
    void* decodedData = qoi_decode(data.data(), static_cast<int>(dataSize), &desc, 0);

    ScopeGuard decodedDataGuard{[decodedData] { free(decodedData); }};

    if (!decodedData) {
        throw invalid_argument{"Failed to decode data from the QOI format."};
    }

    Vector2i size{static_cast<int>(desc.width), static_cast<int>(desc.height)};
    auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    int numChannels = static_cast<int>(desc.channels);
    if (numChannels != 4 && numChannels != 3) {
        throw invalid_argument{tfm::format("Invalid number of channels %d.", numChannels)};
    }

    resultData.channels = makeNChannels(numChannels, size);
    resultData.hasPremultipliedAlpha = false;

    // QOI uses a bitmap 0000rgba for 'colorspace', where a bit 1 indicates linear,
    // however, it is purely informative (meaning it has no effect in en/decoding).
    // Thus, we interpret the default 0x0 value to mean: sRGB encoded RGB channels
    // with linear encoded alpha channel:
    bool isSRGBChannel[4] = {true, true, true, false};
    switch (desc.colorspace) {
        case 0x0: // case QOI_SRGB:
        case QOI_SRGB_LINEAR_ALPHA:
            break;
        case QOI_LINEAR:
            isSRGBChannel[0] = false;
            isSRGBChannel[1] = false;
            isSRGBChannel[2] = false;
            break;
        default:
            // FIXME: should we handle "per-channel" encoding information or just the two cases above?
            // Another option is assuming all values except for QOI_LINEAR mean QOI_SRGB_LINEAR_ALPHA.
            // throw invalid_argument{tfm::format("Unsupported QOI colorspace: %X", desc.colorspace)};
            isSRGBChannel[0] = (desc.colorspace & 0x8) == 0x0; // R channel => 0000rgba & 1000 = r000
            isSRGBChannel[1] = (desc.colorspace & 0x4) == 0x0; // G channel => 0000rgba & 0100 = 0g00
            isSRGBChannel[2] = (desc.colorspace & 0x2) == 0x0; // B channel => 0000rgba & 0010 = 00b0
            isSRGBChannel[3] = (desc.colorspace & 0x1) == 0x0; // A channel => 0000rgba & 0001 = 000a
            break;
    }

    co_await gThreadPool->parallelForAsync<size_t>(0, numPixels, [&](size_t i) {
        auto typedData = reinterpret_cast<unsigned char*>(decodedData);
        size_t baseIdx = i * numChannels;
        for (int c = 0; c < numChannels; ++c) {
            if (isSRGBChannel[c]) {
                resultData.channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
            } else {
                resultData.channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
            }
        }
    }, priority);

    co_return result;
}

TEV_NAMESPACE_END
