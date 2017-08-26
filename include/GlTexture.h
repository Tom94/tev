// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include "../include/Common.h"

#include <nanogui/opengl.h>

class GlTexture {
public:
    GlTexture() = default;

    GlTexture(GLint textureId) : mId(textureId) {}

    GlTexture(const GlTexture& other) = delete;
    GlTexture(GlTexture&& other) noexcept : mId(other.mId) {
        other.mId = 0;
    }

    GlTexture& operator=(const GlTexture& other) = delete;
    GlTexture& operator=(GlTexture&& other) noexcept {
        std::swap(mId, other.mId);
        return *this;
    }

    ~GlTexture() noexcept {
        if (mId) {
            glDeleteTextures(1, &mId);
        }
    }

    auto id() const { return mId; }
    const auto& data() const { return mData; }
    const auto& size() const { return mSize; }

    void setData(const std::vector<float>& data, const Eigen::Vector2i& size, int numChannels);

private:
    GLuint mId = 0;

    Eigen::Vector2i mSize = Eigen::Vector2i::Constant(0);
    int mNumChannels = 0;
    std::vector<float> mData;
};
