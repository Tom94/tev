// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>

#include <istream>

TEV_NAMESPACE_BEGIN

class ClipboardImageLoader : public ImageLoader {
public:
    bool canLoadFile(std::istream& iStream) const override;
    ImageData load(std::istream& iStream, const filesystem::path& path, const std::string& channelSelector) const override;

    std::string name() const override {
        return "clipboard";
    }

    bool hasPremultipliedAlpha() const override {
        return false;
    }
};

TEV_NAMESPACE_END
