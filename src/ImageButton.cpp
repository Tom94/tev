/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/ImageButton.h>

#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>

#include <cctype>

using namespace nanogui;
using namespace std;

namespace tev {

ImageButton::ImageButton(Widget* parent, string_view caption, bool canBeReference) :
    Widget{parent}, mCaption{caption}, mCanBeReference{canBeReference} {
    this->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
    mCaptionTextBox = new TextBox{this, caption};
    mCaptionTextBox->set_visible(false);
    mCaptionTextBox->set_editable(true);
    mCaptionTextBox->set_alignment(TextBox::Alignment::Right);
    mCaptionTextBox->set_placeholder(caption);
    mCaptionTextBox->set_callback([this](const string&) {
        this->hideTextBox();
        return true;
    });
    mCaptionTextBox->set_corner_radius(0.0f);
    mCaptionTextBox->set_solid_color(IMAGE_COLOR);
}

Vector2i ImageButton::preferred_size_impl(NVGcontext* ctx) const {
    if (m_preferred_size_cache != Vector2i(-1)) {
        return m_preferred_size_cache;
    }

    nvgFontSize(ctx, m_font_size);
    nvgFontFace(ctx, "sans-bold");
    string idString = to_string(mId);
    float idSize = nvgTextBounds(ctx, 0, 0, idString.data(), idString.data() + idString.size(), nullptr);

    nvgFontSize(ctx, m_font_size);
    nvgFontFace(ctx, "sans");
    float tw = nvgTextBounds(ctx, 0, 0, mCaption.data(), mCaption.data() + mCaption.size(), nullptr);

    m_preferred_size_cache = Vector2i(static_cast<int>(tw + idSize) + 15, m_font_size + 6);
    return m_preferred_size_cache;
}

bool ImageButton::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    if (Widget::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    if (!m_enabled || !down) {
        return false;
    }

    if (mCanBeReference && (button == GLFW_MOUSE_BUTTON_2 || (button == GLFW_MOUSE_BUTTON_1 && modifiers & GLFW_MOD_SHIFT))) {
        // If we already were the reference, then let's disable using us a reference.
        mIsReference = !mIsReference;

        // If we newly became the reference, then we need to disable the existing reference if it exists.
        if (mIsReference) {
            for (auto widget : parent()->children()) {
                ImageButton* b = dynamic_cast<ImageButton*>(widget);
                if (b && b != this) {
                    b->mIsReference = false;
                }
            }
        }

        // Invoke the callback in any case, such that the surrounding code can react to new references or a loss of a reference image.
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

void ImageButton::draw(NVGcontext* ctx) {
    Widget::draw(ctx);

    if (mCaptionTextBox->visible()) {
        return;
    }

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

    const string idString = to_string(mId);
    if (m_size.x() == preferred_size_impl(ctx).x()) {
        mCutoff = 0;
    } else if (m_size != mSizeForWhichCutoffWasComputed) {
        mCutoff = 0;

        nvgFontSize(ctx, m_font_size + 2);
        nvgFontFace(ctx, "sans-bold");
        const float idSize = nvgTextBounds(ctx, 0, 0, idString.data(), idString.data() + idString.size(), nullptr);

        nvgFontSize(ctx, m_font_size);
        while (mCutoff < mCaption.size()) {
            float bounds = nvgTextBounds(ctx, 0, 0, mCaption.data() + mCutoff, mCaption.data() + mCaption.size(), nullptr);
            if (bounds <= m_size.x() - 25 - idSize) {
                break;
            }

            mCutoff += codePointLength(mCaption[mCutoff]);
        }

        mSizeForWhichCutoffWasComputed = m_size;
    }

    // Image name
    const string_view caption = string_view{mCaption}.substr(mCutoff);
    vector<string_view> pieces;
    if (mHighlightBegin <= mCutoff) {
        if (mHighlightEnd <= mCutoff) {
            pieces.emplace_back(caption);
        } else {
            const size_t offset = mHighlightEnd - mCutoff;
            pieces.emplace_back(caption.substr(offset));
            pieces.emplace_back(caption.substr(0, offset));
        }
    } else {
        const size_t beginOffset = mHighlightBegin - mCutoff;
        const size_t endOffset = mHighlightEnd - mCutoff;
        pieces.emplace_back(caption.substr(endOffset));
        pieces.emplace_back(caption.substr(beginOffset, endOffset - beginOffset));
        pieces.emplace_back(caption.substr(0, beginOffset));
    }

    if (mCutoff > 0 && mCutoff < mCaption.size()) {
        pieces.emplace_back("…");
    }

    const Vector2f center = Vector2f{m_pos} + Vector2f{m_size} * 0.5f;
    const Vector2f bottomRight = Vector2f{m_pos} + Vector2f{m_size};
    Vector2f textPos{bottomRight.x() - 5, center.y() + 0.5f * (m_font_size + 1)};

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
        nvgText(ctx, textPos.x(), textPos.y(), pieces[i].data(), pieces[i].data() + pieces[i].size());
        textPos.x() -= nvgTextBounds(ctx, 0, 0, pieces[i].data(), pieces[i].data() + pieces[i].size(), nullptr);
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
    nvgText(ctx, m_pos.x() + 5, textPos.y(), idString.data(), idString.data() + idString.size());
}

void ImageButton::setHighlightRange(size_t begin, size_t end) {
    size_t beginIndex = begin;
    if (end > mCaption.size()) {
        throw std::invalid_argument{fmt::format("end ({}) must not be larger than mCaption.size() ({})", end, mCaption.size())};
    }

    mHighlightBegin = beginIndex;
    mHighlightEnd = std::max(mCaption.size() - end, beginIndex);

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

void ImageButton::showTextBox() {
    mCaptionTextBox->set_visible(true);
    mCaptionTextBox->request_focus();
    mCaptionTextBox->select_all();
}

void ImageButton::hideTextBox() {
    if (!textBoxVisible()) {
        return;
    }

    mCaptionTextBox->set_focused(false);
    this->setCaption(mCaptionTextBox->value());
    mCaptionTextBox->set_visible(false);
}

} // namespace tev
