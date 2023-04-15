// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/imageio/ImageSaver.h>

#include <ostream>

namespace tev {

class QoiImageSaver : public TypedImageSaver<char> {
public:
    void save(std::ostream& oStream, const fs::path& path, const std::vector<char>& data, const nanogui::Vector2i& imageSize, int nChannels) const override;

    bool hasPremultipliedAlpha() const override {
        return false;
    }

    virtual bool canSaveFile(const std::string& extension) const override {
        return toLower(extension) == ".qoi";
    }
};

}
