// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/QoiImageLoader.h>
#include <tev/ThreadPool.h>

#include <qoi.h>

using namespace Eigen;
using namespace filesystem;
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

// FIXME: this is using `path` directly.
ImageData QoiImageLoader::load(istream& iStream, const path& path, const string& channelSelector, bool& hasPremultipliedAlpha) const {
    ImageData result;

    char magic[4];
    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (magicString != "qoif") {
        throw invalid_argument{tfm::format("Invalid magic QOI string %s", magicString)};
    }

    void* data;
    int numChannels = 4;
    Vector2i size;

    // FIXME: looks like we can't infer whether the image is RGB or RGBA (?)
    data = qoi_read(path.str().c_str(), &size.x(), &size.y(), numChannels);

    if (!data) {
        throw invalid_argument{"Failed to read QOI file."};
    }

    if (size.x() == 0 || size.y() == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    ScopeGuard dataGuard{[data] { free(data); }};

    vector<Channel> channels = makeNChannels(numChannels, size);
    int alphaChannelIndex = 3;

    auto numPixels = (DenseIndex)size.x() * size.y();

    auto typedData = reinterpret_cast<unsigned char*>(data);
    gThreadPool->parallelFor<DenseIndex>(0, numPixels, [&](DenseIndex i) {
        int baseIdx = i * numChannels;
        for (int c = 0; c < numChannels; ++c) {
            if (c == alphaChannelIndex) {
                channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
            } else {
                // TODO: Update this when the final QOI data format is decided.
                // https://github.com/phoboslab/qoi/issues/28
                channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
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

    // QOI can not load layers, so all channels simply reside
    // within a topmost root layer.
    result.layers.emplace_back("");

    // TODO: Update this when the final QOI data format is decided.
    // https://github.com/phoboslab/qoi/issues/28
    hasPremultipliedAlpha = false;

    return result;
}

TEV_NAMESPACE_END
