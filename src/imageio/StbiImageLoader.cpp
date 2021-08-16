// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/StbiImageLoader.h>
#include <tev/ThreadPool.h>

#include <stb_image.h>

using namespace filesystem;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

bool StbiImageLoader::canLoadFile(istream&) const {
    // Pretend you can load any file and throw exception on failure.
    // TODO: Add proper check.
    return true;
}

Task<ImageData> StbiImageLoader::load(istream& iStream, const path&, const string& channelSelector, int priority) const {
    ImageData result;

    static const stbi_io_callbacks callbacks = {
        // Read
        [](void* context, char* data, int size) {
            auto stream = reinterpret_cast<istream*>(context);
            stream->read(data, size);
            return (int)stream->gcount();
        },
        // Seek
        [](void* context, int size) {
            reinterpret_cast<istream*>(context)->seekg(size, ios_base::cur);
        },
        // EOF
        [](void* context) {
            return (int)!!(*reinterpret_cast<istream*>(context));
        },
    };

    void* data;
    int numChannels;
    Vector2i size;
    bool isHdr = stbi_is_hdr_from_callbacks(&callbacks, &iStream) != 0;
    iStream.clear();
    iStream.seekg(0);

    if (isHdr) {
        data = stbi_loadf_from_callbacks(&callbacks, &iStream, &size.x(), &size.y(), &numChannels, 0);
    } else {
        data = stbi_load_from_callbacks(&callbacks, &iStream, &size.x(), &size.y(), &numChannels, 0);
    }

    if (!data) {
        throw invalid_argument{tfm::format("%s", stbi_failure_reason())};
    }

    if (size.x() == 0 || size.y() == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    ScopeGuard dataGuard{[data] { stbi_image_free(data); }};

    result.channels = makeNChannels(numChannels, size);
    int alphaChannelIndex = 3;

    auto numPixels = (size_t)size.x() * size.y();
    if (isHdr) {
        co_await gThreadPool->parallelForAsync<size_t>(0, numPixels, [&](size_t i) {
            auto typedData = reinterpret_cast<float*>(data);
            size_t baseIdx = i * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                result.channels[c].at(i) = typedData[baseIdx + c];
            }
        }, priority);
    } else {
        co_await gThreadPool->parallelForAsync<size_t>(0, numPixels, [&](size_t i) {
            auto typedData = reinterpret_cast<unsigned char*>(data);
            size_t baseIdx = i * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                if (c == alphaChannelIndex) {
                    result.channels[c].at(i) = (typedData[baseIdx + c]) / 255.0f;
                } else {
                    result.channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
                }
            }
        }, priority);
    }

    result.hasPremultipliedAlpha = false;

    co_return result;
}

TEV_NAMESPACE_END
