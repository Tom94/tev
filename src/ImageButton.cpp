// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/ImageButton.h"

#include <nanogui/opengl.h>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageButton::ImageButton(Widget *parent, const string &caption)
    : Widget(parent), mCaption(caption)
{
    mFontSize = 15;
    setTooltip(caption);
}

Vector2i ImageButton::preferredSize(NVGcontext *ctx) const {
    nvgFontSize(ctx, mFontSize);
    nvgFontFace(ctx, "sans-bold");
    float tw = nvgTextBounds(ctx, 0, 0, mCaption.c_str(), nullptr, nullptr);
    return Vector2i(static_cast<int>(tw) + 10, mFontSize + 6);
}

bool ImageButton::mouseButtonEvent(const Vector2i &p, int button, bool down, int modifiers) {
    Widget::mouseButtonEvent(p, button, down, modifiers);

    if (!mEnabled || !down) {
        return false;
    }

    if (button == GLFW_MOUSE_BUTTON_1) {
        if (!mIsSelected) {
            // Unselect the other, currently selected image.
            for (auto widget : parent()->children()) {
                ImageButton* b = dynamic_cast<ImageButton*>(widget);
                if (b && b != this) {
                    b->mIsSelected = false;
                }
            }

            mIsSelected = true;
            if (mSelectedCallback) {
                mSelectedCallback();
            }
        }
        return true;
    } else if (button == GLFW_MOUSE_BUTTON_2) {
        // If we already were the reference, then let's disable using us a reference.
        mIsReference = !mIsReference;

        // If we newly became the reference, then we need to disable the existing reference
        // if it exists.
        if (mIsReference) {
            for (auto widget : parent()->children()) {
                ImageButton* b = dynamic_cast<ImageButton*>(widget);
                if (b && b != this) {
                    b->mIsReference = false;
                }
            }
        }

        // Invoke the callback in any case, such that the surrounding code can
        // react to new references or a loss of a reference image.
        if (mReferenceCallback) {
            mReferenceCallback(mIsReference);
        }
        return true;
    }
    return false;
}

void ImageButton::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    NVGcolor color = Color(0.35f, 0.35f, 0.35f, 1.0f);

    if (mIsSelected) {
        color = Color(0.4f, 0.4f, 0.7f, 1.0f);
    } else if (mMouseFocus && mEnabled) {
        color = Color(0.4f, 0.4f, 0.4f, 1.0f);
    }

    if (mIsReference) {
        nvgBeginPath(ctx);
        nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
        nvgFillColor(ctx, Color(0.7f, 0.4f, 0.4f, 1.0f));
        nvgFill(ctx);
    }

    nvgBeginPath(ctx);

    if (mIsReference) {
        nvgRect(ctx, mPos.x() + 2, mPos.y() + 2, mSize.x() - 4, mSize.y() - 4);
    } else {
        nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
    }

    nvgFillColor(ctx, color);
    nvgFill(ctx);

    nvgFontSize(ctx, mFontSize);
    nvgFontFace(ctx, "sans-bold");

    string caption = mCaption;
    while (nvgTextBounds(ctx, 0, 0, caption.c_str(), nullptr, nullptr) > mSize.x() - 15) {
        caption = caption.substr(1, caption.length() - 1);
    }

    if (caption.length() != mCaption.length()) {
        caption = "…"s + caption;
    }

    Vector2f center = mPos.cast<float>() + mSize.cast<float>() * 0.5f;
    Vector2f bottomRight = mPos.cast<float>() + mSize.cast<float>();
    Vector2f textPos(bottomRight.x() - 5, center.y() - 1);
    NVGcolor textColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
    if (!mEnabled) {
        textColor = Color(0.7f, 0.7f, 0.7f, 1.0f);
    }

    nvgFontSize(ctx, mFontSize);
    nvgFontFace(ctx, "sans");
    nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(ctx, Color(0.2f, 0.2f, 0.2f, 0.2f));
    nvgText(ctx, textPos.x(), textPos.y(), caption.c_str(), nullptr);
    nvgFillColor(ctx, textColor);
    nvgText(ctx, textPos.x(), textPos.y() + 1, caption.c_str(), nullptr);
}

TEV_NAMESPACE_END
