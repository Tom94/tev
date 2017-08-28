// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/ImageCanvas.h"

using namespace Eigen;
using namespace nanogui;

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

    if (mReference) {
        mShader.draw(
            {{
                mImage->hasChannel("R") ? mImage->texture("R") : &mTextureBlack,
                mImage->hasChannel("G") ? mImage->texture("G") : &mTextureBlack,
                mImage->hasChannel("B") ? mImage->texture("B") : &mTextureBlack,
                mImage->hasChannel("A") ? mImage->texture("A") : &mTextureWhite,
            }},
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(mImage.get()).inverse(),
            {{
                (mReference && mReference->hasChannel("R")) ? mReference->texture("R") : &mTextureBlack,
                (mReference && mReference->hasChannel("G")) ? mReference->texture("G") : &mTextureBlack,
                (mReference && mReference->hasChannel("B")) ? mReference->texture("B") : &mTextureBlack,
                (mReference && mReference->hasChannel("A")) ? mReference->texture("A") : &mTextureWhite,
            }},
            transform(mImage.get()).inverse(),
            mExposure,
            mTonemap,
            mMetric
        );
    } else {
        mShader.draw(
            {{
                mImage->hasChannel("R") ? mImage->texture("R") : &mTextureBlack,
                mImage->hasChannel("G") ? mImage->texture("G") : &mTextureBlack,
                mImage->hasChannel("B") ? mImage->texture("B") : &mTextureBlack,
                mImage->hasChannel("A") ? mImage->texture("A") : &mTextureWhite,
            }},
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(mImage.get()).inverse(),
            mExposure,
            mTonemap
        );
    }
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
