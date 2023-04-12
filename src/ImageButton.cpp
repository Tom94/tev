// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ImageButton.h>

#include <nanogui/opengl.h>

#include <cctype>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageButton::ImageButton(Widget *parent, const string &caption, bool canBeReference)
: Widget{parent}, mCaption{caption}, mCanBeReference{canBeReference} {
}

Vector2i ImageButton::preferred_size(NVGcontext *ctx) const {
    nvgFontSize(ctx, m_font_size);
    nvgFontFace(ctx, "sans-bold");
    string idString = to_string(mId);
    float idSize = nvgTextBounds(ctx, 0, 0, idString.c_str(), nullptr, nullptr);

    nvgFontSize(ctx, m_font_size);
    nvgFontFace(ctx, "sans");
    float tw = nvgTextBounds(ctx, 0, 0, mCaption.c_str(), nullptr, nullptr);
    return Vector2i(static_cast<int>(tw + idSize) + 15, m_font_size + 6);
}

bool ImageButton::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    if (Widget::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    if (!m_enabled || !down) {
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
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, REFERENCE_COLOR);
        nvgFill(ctx);
    }

    // Fill the button with color.
    if (mIsSelected || m_mouse_focus) {
        nvgBeginPath(ctx);

        if (mIsReference) {
            nvgRect(ctx, m_pos.x() + 2, m_pos.y() + 2, m_size.x() - 4, m_size.y() - 4);
        } else {
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        }

        nvgFillColor(ctx, mIsSelected ? IMAGE_COLOR : Color(1.0f, 0.1f));
        nvgFill(ctx);
    }


    string idString = to_string(mId);

    if (m_size.x() == preferred_size(ctx).x()) {
        mCutoff = 0;
    } else if (m_size != mSizeForWhichCutoffWasComputed) {
        mCutoff = 0;

        nvgFontSize(ctx, m_font_size + 2);
        nvgFontFace(ctx, "sans-bold");
        float idSize = nvgTextBounds(ctx, 0, 0, idString.c_str(), nullptr, nullptr);

        nvgFontSize(ctx, m_font_size);
        while (mCutoff < mCaption.size() && nvgTextBounds(ctx, 0, 0, mCaption.substr(mCutoff).c_str(), nullptr, nullptr) > m_size.x() - 25 - idSize) {
            mCutoff += codePointLength(mCaption[mCutoff]);;
        }

        mSizeForWhichCutoffWasComputed = m_size;
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
        pieces.back() = "…"s + pieces.back();
    }

    Vector2f center = Vector2f{m_pos} + Vector2f{m_size} * 0.5f;
    Vector2f bottomRight = Vector2f{m_pos} + Vector2f{m_size};
    Vector2f textPos(bottomRight.x() - 5, center.y() + 0.5f * (m_font_size + 1));
    NVGcolor regularTextColor = mCanBeReference ? Color(150, 255) : Color(190, 255);
    NVGcolor hightlightedTextColor = Color(190, 255);
    if (mIsSelected || mIsReference || m_mouse_focus) {
        regularTextColor = hightlightedTextColor = Color(255, 255);
    }

    nvgFontSize(ctx, m_font_size);
    nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);

    for (size_t i = 0; i < pieces.size(); ++i) {
        nvgFontFace(ctx, i == 1 ? "sans-bold" : "sans");
        nvgFillColor(ctx, i == 1 ? hightlightedTextColor : regularTextColor);
        nvgText(ctx, textPos.x(), textPos.y(), pieces[i].c_str(), nullptr);
        textPos.x() -= nvgTextBounds(ctx, 0, 0, pieces[i].c_str(), nullptr, nullptr);
    }

    // Image number
    NVGcolor idColor = Color(200, 255);
    if (mIsSelected || mIsReference || m_mouse_focus) {
        idColor = Color(255, 255);
    }

    nvgFontSize(ctx, m_font_size + 2);
    nvgFontFace(ctx, "sans-bold");
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
    nvgFillColor(ctx, idColor);
    nvgText(ctx, m_pos.x() + 5, textPos.y(), idString.c_str(), nullptr);
}

void ImageButton::setHighlightRange(size_t begin, size_t end) {
    size_t beginIndex = begin;
    if (end > mCaption.size()) {
        throw std::invalid_argument{format(
            "end ({}) must not be larger than mCaption.size() ({})",
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
