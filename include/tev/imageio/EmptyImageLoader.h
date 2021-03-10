// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>

#include <istream>

TEV_NAMESPACE_BEGIN

class EmptyImageLoader : public ImageLoader {
public:
    bool canLoadFile(std::istream& iStream) const override;
    ImageData load(std::istream& iStream, const filesystem::path& path, const std::string& channelSelector, bool& hasPremultipliedAlpha) const override;

    std::string name() const override {
        return "IPC";
    }
};

TEV_NAMESPACE_END
