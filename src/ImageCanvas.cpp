// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/ImageCanvas.h"

using namespace Eigen;
using namespace nanogui;

ImageCanvas::ImageCanvas(nanogui::Widget* parent, float pixelRatio)
    : GLCanvas(parent), mPixelRatio(pixelRatio) {

}

bool ImageCanvas::mouseMotionEvent(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    if (GLCanvas::mouseMotionEvent(p, rel, button, modifiers)) {
        return true;
    }

    // If left mouse button is held
    if ((button & 1) != 0) {
        mTransform = Translation2f(rel.cast<float>()) * mTransform;
    }

    return true;
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

    mShader.draw(
        {{
            mImage->texture("R"),
            mImage->texture("G"),
            mImage->texture("B"),
        }},
        mExposure,
        imageTransform()
    );
}

Matrix3f ImageCanvas::imageTransform() {
    Vector2f imageSize = mImage->size().cast<float>();

    // Center image, scale to pixel space, translate to desired position,
    // then rescale to the [-1, 1] square for drawing.
    return (
        Scaling(2.0f / mSize.x(), -2.0f / mSize.y()) *
        mTransform *
        Scaling(imageSize / mPixelRatio) *
        Translation2f(Vector2f::Constant(-0.5f))
    ).matrix();
}
