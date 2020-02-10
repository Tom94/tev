// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Channel.h>
#include <tev/Image.h>

#include <istream>
#include <string>

TEV_NAMESPACE_BEGIN

class ImageLoader {
public:
    virtual ~ImageLoader() {}

    virtual bool canLoadFile(std::istream& iStream) const = 0;
    virtual ImageData load(std::istream& iStream, const filesystem::path& path, const std::string& channelSelector) const = 0;

    virtual std::string name() const = 0;

    virtual bool hasPremultipliedAlpha() const = 0;

    static const std::vector<std::unique_ptr<ImageLoader>>& getLoaders();

protected:
    static std::vector<Channel> makeNChannels(int numChannels, Eigen::Vector2i size);
};

TEV_NAMESPACE_END
