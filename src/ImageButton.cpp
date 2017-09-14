// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/ImageButton.h"

#include <nanogui/opengl.h>

#include <cctype>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageButton::ImageButton(Widget *parent, const string &caption, bool canBeReference)
: Widget{parent}, mCaption{caption}, mCanBeReference{canBeReference} {
}

Vector2i ImageButton::preferredSize(NVGcontext *ctx) const {
    nvgFontSize(ctx, mFontSize);
    nvgFontFace(ctx, "sans-bold");
    string idString = to_string(mId);
    float idSize = nvgTextBounds(ctx, 0, 0, idString.c_str(), nullptr, nullptr);

    nvgFontSize(ctx, mFontSize);
    nvgFontFace(ctx, "sans");
    float tw = nvgTextBounds(ctx, 0, 0, mCaption.c_str(), nullptr, nullptr);
    return Vector2i(static_cast<int>(tw + idSize) + 15, mFontSize + 6);
}

bool ImageButton::mouseButtonEvent(const Vector2i &p, int button, bool down, int modifiers) {
    if (Widget::mouseButtonEvent(p, button, down, modifiers)) {
        return true;
    }

    if (!mEnabled || !down) {
        return false;
    }

    if (mCanBeReference && (button == GLFW_MOUSE_BUTTON_2 || (button == GLFW_MOUSE_BUTTON_1 && modifiers & GLFW_MOD_SHIFT))) {
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
    } else if (button == GLFW_MOUSE_BUTTON_1) {
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
    }

    return false;
}

void ImageButton::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    if (mIsReference) {
        nvgBeginPath(ctx);
        nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
        nvgFillColor(ctx, Color(0.7f, 0.4f, 0.4f, 1.0f));
        nvgFill(ctx);
    }

    // Fill the button with color.
    if (mIsSelected || mMouseFocus) {
        nvgBeginPath(ctx);

        if (mIsReference) {
            nvgRect(ctx, mPos.x() + 2, mPos.y() + 2, mSize.x() - 4, mSize.y() - 4);
        } else {
            nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
        }

        nvgFillColor(ctx, mIsSelected ? Color(0.35f, 0.35f, 0.8f, 1.0f) : Color(1.0f, 0.1f));
        nvgFill(ctx);
    }


    string idString = to_string(mId);

    if (mSize.x() == preferredSize(ctx).x()) {
        mCutoff = 0;
    } else if (mSize != mSizeForWhichCutoffWasComputed) {
        mCutoff = 0;

        nvgFontSize(ctx, mFontSize + 2);
        nvgFontFace(ctx, "sans-bold");
        float idSize = nvgTextBounds(ctx, 0, 0, idString.c_str(), nullptr, nullptr);

        nvgFontSize(ctx, mFontSize);
        while (mCutoff < mCaption.size() && nvgTextBounds(ctx, 0, 0, mCaption.substr(mCutoff).c_str(), nullptr, nullptr) > mSize.x() - 25 - idSize) {
            ++mCutoff;
        }

        mSizeForWhichCutoffWasComputed = mSize;
    }

    // Image name
    string caption = mCaption.substr(mCutoff);

    vector<string> pieces;
    if (mHighlightBegin <= mCutoff) {
        if (mHighlightEnd <= mCutoff) {
            pieces.emplace_back(caption);
        } else {
            size_t offset = mHighlightEnd - mCutoff;
            pieces.emplace_back(caption.substr(offset));
            pieces.emplace_back(caption.substr(0, offset));
        }
    } else {
        size_t beginOffset = mHighlightBegin - mCutoff;
        size_t endOffset = mHighlightEnd - mCutoff;
        pieces.emplace_back(caption.substr(endOffset));
        pieces.emplace_back(caption.substr(beginOffset, endOffset - beginOffset));
        pieces.emplace_back(caption.substr(0, beginOffset));
    }

    if (mCutoff > 0 && mCutoff < mCaption.size()) {
        pieces.back() = string{"…"} + pieces.back();
    }

    Vector2f center = mPos.cast<float>() + mSize.cast<float>() * 0.5f;
    Vector2f bottomRight = mPos.cast<float>() + mSize.cast<float>();
    Vector2f textPos(bottomRight.x() - 5, center.y() + 0.5f * (mFontSize + 1));
    NVGcolor regularTextColor = mCanBeReference ? Color(150, 255) : Color(190, 255);
    NVGcolor hightlightedTextColor = Color(190, 255);
    if (mIsSelected || mIsReference || mMouseFocus) {
        regularTextColor = hightlightedTextColor = Color(255, 255);
    }

    nvgFontSize(ctx, mFontSize);
    nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
    
    for (size_t i = 0; i < pieces.size(); ++i) {
        nvgFontFace(ctx, i == 1 ? "sans-bold" : "sans");
        nvgFillColor(ctx, i == 1 ? hightlightedTextColor : regularTextColor);
        nvgText(ctx, textPos.x(), textPos.y(), pieces[i].c_str(), nullptr);
        textPos.x() -= nvgTextBounds(ctx, 0, 0, pieces[i].c_str(), nullptr, nullptr);
    }

    // Image number
    NVGcolor idColor = Color(200, 255);
    if (mIsSelected || mIsReference || mMouseFocus) {
        idColor = Color(255, 255);
    }

    nvgFontSize(ctx, mFontSize + 2);
    nvgFontFace(ctx, "sans-bold");
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
    nvgFillColor(ctx, idColor);
    nvgText(ctx, mPos.x() + 5, textPos.y(), idString.c_str(), nullptr);
}

void ImageButton::setHighlightRange(size_t begin, size_t end) {
    size_t beginIndex = begin;
    if (end > mCaption.size()) {
        throw std::invalid_argument{tfm::format(
            "end (%d) must not be larger than mCaption.size() (%d)",
            end, mCaption.size()
        )};
    }

    mHighlightBegin = beginIndex;
    mHighlightEnd = max(mCaption.size() - end, beginIndex);

    if (mHighlightBegin == mHighlightEnd || mCaption.empty()) {
        return;
    }

    // Extend beginning and ending of highlighted region to entire word/number
    if (isalnum(mCaption[mHighlightBegin])) {
        while (mHighlightBegin > 0 && isalnum(mCaption[mHighlightBegin - 1])) {
            --mHighlightBegin;
        }
    }

    if (isalnum(mCaption[mHighlightEnd - 1])) {
        while (mHighlightEnd < mCaption.size() && isalnum(mCaption[mHighlightEnd])) {
            ++mHighlightEnd;
        }
    }
}

TEV_NAMESPACE_END
