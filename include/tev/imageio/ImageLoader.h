// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Channel.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>

#include <nanogui/vector.h>

#include <istream>
#include <tuple>
#include <string>

namespace tev {

class ImageLoader {
public:
    virtual ~ImageLoader() {}

    virtual bool canLoadFile(std::istream& iStream) const = 0;

    // Return loaded image data as well as whether that data has the alpha channel pre-multiplied or not.
    virtual Task<std::vector<ImageData>> load(std::istream& iStream, const fs::path& path, const std::string& channelSelector, int priority) const = 0;

    virtual std::string name() const = 0;

    static const std::vector<std::unique_ptr<ImageLoader>>& getLoaders();

protected:
    static std::vector<Channel> makeNChannels(int numChannels, const nanogui::Vector2i& size);
};

}
