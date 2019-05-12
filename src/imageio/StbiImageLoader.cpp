// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/StbiImageLoader.h>
#include <tev/ThreadPool.h>

#include <stb_image.h>

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

bool StbiImageLoader::canLoadFile(ifstream&) const {
    // Pretend you can load any file and throw exception on failure.
    // TODO: Add proper check.
    return true;
}

ImageData StbiImageLoader::load(ifstream& f, const filesystem::path&, const string& channelSelector) const {
    ImageData result;
    ThreadPool threadPool;

    static const stbi_io_callbacks callbacks = {
        // Read
        [](void* context, char* data, int size) {
            auto stream = reinterpret_cast<ifstream*>(context);
            stream->read(data, size);
            return (int)stream->gcount();
        },
        // Seek
        [](void* context, int size) {
            reinterpret_cast<ifstream*>(context)->seekg(size, ios_base::cur);
        },
        // EOF
        [](void* context) {
            return (int)!!(*reinterpret_cast<ifstream*>(context));
        },
    };

    void* data;
    int numChannels;
    Vector2i size;
    bool isHdr = stbi_is_hdr_from_callbacks(&callbacks, &f) != 0;
    f.seekg(0);

    if (isHdr) {
        data = stbi_loadf_from_callbacks(&callbacks, &f, &size.x(), &size.y(), &numChannels, 0);
    } else {
        data = stbi_load_from_callbacks(&callbacks, &f, &size.x(), &size.y(), &numChannels, 0);
    }

    if (!data) {
        throw invalid_argument{"Could not load texture data"};
    }

    ScopeGuard dataGuard{[data] { stbi_image_free(data); }};

    vector<Channel> channels = makeNChannels(numChannels, size);

    auto numPixels = (DenseIndex)size.x() * size.y();
    if (isHdr) {
        auto typedData = reinterpret_cast<float*>(data);
        threadPool.parallelFor<DenseIndex>(0, numPixels, [&](DenseIndex i) {
            int baseIdx = i * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                channels[c].at(i) = typedData[baseIdx + c];
            }
        });
    } else {
        auto typedData = reinterpret_cast<unsigned char*>(data);
        threadPool.parallelFor<DenseIndex>(0, numPixels, [&](DenseIndex i) {
            int baseIdx = i * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
            }
        });
    }

    for (auto& channel : channels) {
        string name = channel.name();
        if (matchesFuzzy(name, channelSelector)) {
            result.channels.emplace(move(name), move(channel));
        }
    }

    // STBI can not load layers, so all channels simply reside
    // within a topmost root layer.
    result.layers.emplace_back("");

    return result;
}

TEV_NAMESPACE_END
