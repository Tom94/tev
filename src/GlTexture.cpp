// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/GlTexture.h>

#include <iostream>

using namespace std;
using namespace Eigen;

TEV_NAMESPACE_BEGIN

GlTexture::GlTexture(GLint clamping, GLint filtering, bool mipmap)
: mClamping{clamping}, mFiltering{filtering}, mMipmap{mipmap} {
}

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
        data.size() == (size_t)mSize.x() * mSize.y() * numChannels,
        "Supplied data (%d) does not match the size of the texture (%dx%dx%d == %d).",
        data.size(), mSize.x(), mSize.y(), numChannels, mSize.prod()
    );

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, size.x(), size.y(), 0, format, GL_FLOAT, data.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mClamping);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mClamping);

    if (mMipmap) {
        mRequiresMipmapping = true;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mFiltering);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mFiltering);
}

void GlTexture::setDataSub(const vector<float>& data, const Vector2i& origin, const Vector2i& size, int numChannels) {
    TEV_ASSERT(mId != 0, "Cannot set sub data of a texture that does not exist.");
    TEV_ASSERT(mNumChannels == numChannels, "Sub data must have the same number of channels as the entire texture.");
    TEV_ASSERT(
        origin.x() >= 0 && origin.y() >= 0 && origin.x() + size.x() <= mSize.x() && origin.y() + size.y() <= mSize.y(),
        "Sub data tile must fit into the size of the original texture."
    );

    glBindTexture(GL_TEXTURE_2D, mId);
    GLint format;
    switch (numChannels) {
        case 1: format = GL_RED; break;
        case 2: format = GL_RG; break;
        case 3: format = GL_RGB; break;
        case 4: format = GL_RGBA; break;
        default: format = 0; break;
    }

    TEV_ASSERT(
        data.size() == (size_t)size.x() * size.y() * numChannels,
        "Supplied data (%d) does not match the size of the texture (%dx%dx%d == %d).",
        data.size(), size.x(), size.y(), numChannels, size.prod()
    );

    glTexSubImage2D(GL_TEXTURE_2D, 0, origin.x(), origin.y(), size.x(), size.y(), format, GL_FLOAT, data.data());

    // Regenerate the mipmap... this is probably the most expensive part about updating the texture
    if (mMipmap) {
        mRequiresMipmapping = true;
    }
}

void GlTexture::bind() {
    glBindTexture(GL_TEXTURE_2D, mId);

    if (mRequiresMipmapping) {
        glGenerateMipmap(GL_TEXTURE_2D);
        mRequiresMipmapping = false;
    }
}

TEV_NAMESPACE_END
