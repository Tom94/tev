// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/ImageCanvas.h"

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageCanvas::ImageCanvas(nanogui::Widget* parent, float pixelRatio)
: GLCanvas(parent), mPixelRatio(pixelRatio) {
    mTextureBlack.setData({ 0.0 }, Vector2i::Constant(1), 1);
    mTextureWhite.setData({ 1.0 }, Vector2i::Constant(1), 1);
}

bool ImageCanvas::mouseMotionEvent(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    if (GLCanvas::mouseMotionEvent(p, rel, button, modifiers)) {
        return true;
    }

    // If left mouse button is held
    if ((button & 1) != 0) {
        mTransform = Translation2f(rel.cast<float>()) * mTransform;
    }

    return false;
}

bool ImageCanvas::scrollEvent(const Vector2i& p, const Vector2f& rel) {
    if (GLCanvas::scrollEvent(p, rel)) {
        return true;
    }

    float scaleFactor = pow(1.1f, rel.y());

    // Use the current cursor position as the origin to scale around.
    Vector2f offset = -(p - position()).cast<float>() + 0.5f * mSize.cast<float>();
    auto scaleTransform =
        Translation2f(-offset) *
        Scaling(scaleFactor) *
        Translation2f(offset);

    mTransform = scaleTransform * mTransform;

    return true;
}

void ImageCanvas::drawGL() {
    mCheckerboardShader.draw(
        2.0f * mSize.cast<float>().cwiseInverse() / mPixelRatio,
        Vector2f::Constant(20)
    );

    if (!mImage) {
        return;
    }

    auto getTextures = [this](Image& image) {
        const auto& imageChannels = getChannels(image);
        return array<const GlTexture*, 4>{{
            imageChannels.size() > 0 ? image.texture(imageChannels[0]) : &mTextureBlack,
            imageChannels.size() > 1 ? image.texture(imageChannels[1]) : &mTextureBlack,
            imageChannels.size() > 2 ? image.texture(imageChannels[2]) : &mTextureBlack,
            imageChannels.size() > 3 ? image.texture(imageChannels[3]) : &mTextureWhite,
        }};
    };

    if (mReference) {
        mShader.draw(
            getTextures(*mImage),
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(mImage.get()).inverse(),
            getTextures(*mReference),
            transform(mImage.get()).inverse(),
            mExposure,
            mTonemap,
            mMetric
        );
    } else {
        mShader.draw(
            getTextures(*mImage),
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(mImage.get()).inverse(),
            mExposure,
            mTonemap
        );
    }
}

vector<string> ImageCanvas::getChannels(const Image& image) {
    vector<vector<string>> groups = {
        { "R", "G", "B" },
        { "r", "g", "b" },
        { "X", "Y", "Z" },
        { "x", "y", "z" },
        { "U", "V" },
        { "u", "v" },
        { "Z" },
        { "z" },
    };

    string layerPrefix = mRequestedLayer.empty() ? "" : (mRequestedLayer + ".");

    vector<string> result;
    for (const auto& group : groups) {
        for (size_t i = 0; i < group.size(); ++i) {
            const auto& name = layerPrefix + group[i];
            if (image.hasChannel(name)) {
                result.emplace_back(name);
            }
        }

        if (!result.empty()) {
            break;
        }
    }

    string alphaChannelName = layerPrefix + "A";

    // No channels match the given groups; fall back to the first 3 channels.
    if (result.empty()) {
        const auto& channelNames = image.channelsInLayer(mRequestedLayer);
        for (const auto& name : channelNames) {
            if (name != alphaChannelName) {
                result.emplace_back(name);
            }

            if (result.size() >= 3) {
                break;
            }
        }
    }

    // If we found just 1 channel, let's display is as grayscale
    // by duplicating it twice.
    if (result.size() == 1) {
        result.emplace_back(result[0]);
        result.emplace_back(result[0]);
    }

    // If there is an alpha layer, use it
    if (image.hasChannel(alphaChannelName)) {
        result.emplace_back(alphaChannelName);
    }

    return result;
}

Matrix3f ImageCanvas::transform(const Image* image) {
    if (!image) {
        return Matrix3f::Identity();
    }

    // Center image, scale to pixel space, translate to desired position,
    // then rescale to the [-1, 1] square for drawing.
    return (
        Scaling(2.0f / mSize.x(), -2.0f / mSize.y()) *
        mTransform *
        Scaling(image->size().cast<float>() / mPixelRatio) *
        Translation2f(Vector2f::Constant(-0.5f))
    ).matrix();
}

TEV_NAMESPACE_END
