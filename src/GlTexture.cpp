// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/GlTexture.h"

#include <iostream>

using namespace std;
using namespace Eigen;

TEV_NAMESPACE_BEGIN

void GlTexture::setData(const vector<float>& data, const Vector2i& size, int numChannels) {
    if (mId) {
        glDeleteTextures(1, &mId);
        mId = 0;
    }

    mSize = size;
    mNumChannels = numChannels;

    glGenTextures(1, &mId);
    glBindTexture(GL_TEXTURE_2D, mId);
    GLint internalFormat;
    GLint format;
    switch (numChannels) {
        case 1: internalFormat = GL_R32F; format = GL_RED; break;
        case 2: internalFormat = GL_RG32F; format = GL_RG; break;
        case 3: internalFormat = GL_RGB32F; format = GL_RGB; break;
        case 4: internalFormat = GL_RGBA32F; format = GL_RGBA; break;
        default: internalFormat = 0; format = 0; break;
    }

    TEV_ASSERT(
        data.size() == static_cast<size_t>(mSize.prod()),
        "Supplied data (%d) does not match the size of the texture (%dx%d == %d).",
        data.size(), mSize.x(), mSize.y(), mSize.prod()
    );

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, size.x(), size.y(), 0, format, GL_FLOAT, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

TEV_NAMESPACE_END
