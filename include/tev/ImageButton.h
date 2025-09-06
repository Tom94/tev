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

#pragma once

#include <tev/Common.h>

#include <nanogui/textbox.h>
#include <nanogui/widget.h>

#include <string>

namespace tev {

class ImageButton : public nanogui::Widget {
public:
    ImageButton(nanogui::Widget* parent, std::string_view caption, bool canBeReference);

    nanogui::Vector2i preferred_size(NVGcontext* ctx) const override;

    bool mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) override;

    void draw(NVGcontext* ctx) override;

    void set_theme(nanogui::Theme* theme) override {
        nanogui::Widget::set_theme(theme);

        nanogui::Theme* captionTextBoxTheme = new nanogui::Theme(*theme);
        captionTextBoxTheme->m_text_box_font_size = m_font_size;
        captionTextBoxTheme->m_text_color = nanogui::Color(255, 255);
        mCaptionTextBox->set_theme(captionTextBoxTheme);
    }

    std::string_view caption() const { return mCaption; }

    void setCaption(std::string_view caption) {
        mCaption = caption;

        // Reset drawing state
        mSizeForWhichCutoffWasComputed = {0};
        mHighlightBegin = 0;
        mHighlightEnd = 0;

        if (mCaptionChangeCallback) {
            mCaptionChangeCallback();
        }
    }

    void setReferenceCallback(const std::function<void(bool)>& callback) { mReferenceCallback = callback; }

    void setIsReference(bool isReference) { mIsReference = isReference; }

    bool isReference() const { return mIsReference; }

    void setSelectedCallback(const std::function<void()>& callback) { mSelectedCallback = callback; }

    void setCaptionChangeCallback(const std::function<void()>& callback) { mCaptionChangeCallback = callback; }

    void setIsSelected(bool isSelected) { mIsSelected = isSelected; }

    bool isSelected() const { return mIsSelected; }

    void setId(size_t id) { mId = id; }

    size_t id() const { return mId; }

    void setHighlightRange(size_t begin, size_t end);

    void showTextBox();
    void hideTextBox();

    bool textBoxVisible() const { return mCaptionTextBox->visible(); }

private:
    std::string mCaption;
    nanogui::TextBox* mCaptionTextBox;

    bool mCanBeReference;

    bool mIsReference = false;
    std::function<void(bool)> mReferenceCallback;

    bool mIsSelected = false;
    std::function<void()> mSelectedCallback;

    std::function<void()> mCaptionChangeCallback;

    size_t mId = 0;
    size_t mCutoff = 0;
    nanogui::Vector2i mSizeForWhichCutoffWasComputed = {0};

    size_t mHighlightBegin = 0;
    size_t mHighlightEnd = 0;

    mutable size_t mLastSizingId = 0;
    mutable std::string mLastSizingCaption;
    mutable nanogui::Vector2i mLastSizingResult = {0};
};

} // namespace tev
