// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/imageio/ImageSaver.h>

#include <ostream>

TEV_NAMESPACE_BEGIN

class QoiImageSaver : public TypedImageSaver<char> {
public:
    void save(std::ostream& oStream, const filesystem::path& path, const std::vector<char>& data, const nanogui::Vector2i& imageSize, int nChannels) const override;

    bool hasPremultipliedAlpha() const override {
        // TODO: Update this when the final QOI data format is decided.
        // https://github.com/phoboslab/qoi/issues/37
        return false;
    }

    virtual bool canSaveFile(const std::string& extension) const override {
        std::string lowerExtension = toLower(extension);
        return lowerExtension == "qoi";
    }
};

TEV_NAMESPACE_END
