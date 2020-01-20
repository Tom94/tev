// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>

#include <fstream>

TEV_NAMESPACE_BEGIN

class PfmImageLoader : public ImageLoader {
public:
    bool canLoadFile(std::ifstream& f) const override;
    ImageData load(std::ifstream& f, const filesystem::path& path, const std::string& channelSelector) const override;

    std::string name() const override {
        return "PFM";
    }

    bool hasPremultipliedAlpha() const override {
        return false;
    }
};

TEV_NAMESPACE_END
