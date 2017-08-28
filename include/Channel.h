// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/GlTexture.h"

#include <vector>
#include <string>

TEV_NAMESPACE_BEGIN

class Channel {
public:
    Channel(const std::string& name);

    const auto& name() const {
        return mName;
    }

    auto& data() {
        return mData;
    }

    const auto& data() const {
        return mData;
    }

private:
    std::string mName;
    std::vector<float> mData;
};

TEV_NAMESPACE_END
