// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ClipboardImageLoader.h>
#include <tev/imageio/EmptyImageLoader.h>
#include <tev/imageio/ExrImageLoader.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/PfmImageLoader.h>
#include <tev/imageio/QoiImageLoader.h>
#include <tev/imageio/StbiImageLoader.h>
#ifdef _WIN32
#   include <tev/imageio/DdsImageLoader.h>
#endif
#ifdef TEV_USE_LIBHEIF
#   include <tev/imageio/HeifImageLoader.h>
#endif

using namespace nanogui;
using namespace std;

namespace tev {

const vector<unique_ptr<ImageLoader>>& ImageLoader::getLoaders() {
    auto makeLoaders = [] {
        vector<unique_ptr<ImageLoader>> imageLoaders;
        imageLoaders.emplace_back(new ExrImageLoader());
        imageLoaders.emplace_back(new PfmImageLoader());
        imageLoaders.emplace_back(new ClipboardImageLoader());
        imageLoaders.emplace_back(new EmptyImageLoader());
#ifdef _WIN32
        imageLoaders.emplace_back(new DdsImageLoader());
#endif
#ifdef TEV_USE_LIBHEIF
        imageLoaders.emplace_back(new HeifImageLoader());
#endif
        imageLoaders.emplace_back(new QoiImageLoader());
        imageLoaders.emplace_back(new StbiImageLoader());
        return imageLoaders;
    };

    static const vector imageLoaders = makeLoaders();
    return imageLoaders;
}

vector<Channel> ImageLoader::makeNChannels(int numChannels, const Vector2i& size) {
    vector<Channel> channels;

    size_t numPixels = (size_t)size.x() * size.y();
    shared_ptr<vector<float>> data = make_shared<vector<float>>(numPixels * 4);

    // Initialize pattern [0,0,0,1] efficiently using 128-bit writes
    float* ptr = data->data();
    const float pattern[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < numPixels; ++i) {
        memcpy(ptr + i * 4, pattern, 16);
    }

    if (numChannels > 1) {
        const vector<string> channelNames = {"R", "G", "B", "A"};
        for (int c = 0; c < numChannels; ++c) {
            string name = c < (int)channelNames.size() ? channelNames[c] : to_string(c);

            // We assume that the channels are interleaved.
            channels.emplace_back(name, size, data, c, 4);
        }
    } else {
        channels.emplace_back("L", size, data, 0, 4);
    }

    return channels;
}

} // namespace tev
