// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <nanogui/widget.h>
#include <nanogui/textbox.h>

#include <string>

namespace tev {

class ImageButton : public nanogui::Widget {
public:
    ImageButton(nanogui::Widget* parent, const std::string& caption, bool canBeReference);

    nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;

    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down, int modifiers) override;

    void draw(NVGcontext *ctx) override;

    void set_theme(nanogui::Theme* theme) override {
        nanogui::Widget::set_theme(theme);

        nanogui::Theme* captionTextBoxTheme = new nanogui::Theme(*theme);
        captionTextBoxTheme->m_text_box_font_size = m_font_size;
        captionTextBoxTheme->m_text_color = nanogui::Color(255, 255);
        mCaptionTextBox->set_theme(captionTextBoxTheme);
    }

    const std::string& caption() const {
        return mCaption;
    }

    void setCaption(const std::string& caption) {
        mCaption = caption;
        // Reset drawing state
        mSizeForWhichCutoffWasComputed = {0};
        mHighlightBegin = 0;
        mHighlightEnd = 0;

        if (mCaptionChangeCallback) {
            mCaptionChangeCallback();
        }
    }

    void setReferenceCallback(const std::function<void(bool)> &callback) {
        mReferenceCallback = callback;
    }

    void setIsReference(bool isReference) {
        mIsReference = isReference;
    }

    bool isReference() const {
        return mIsReference;
    }

    void setSelectedCallback(const std::function<void()> &callback) {
        mSelectedCallback = callback;
    }

    void setCaptionChangeCallback(const std::function<void()> &callback) {
        mCaptionChangeCallback = callback;
    }

    void setIsSelected(bool isSelected) {
        mIsSelected = isSelected;
    }

    bool isSelected() const {
        return mIsSelected;
    }

    void setId(size_t id) {
        mId = id;
    }

    void setHighlightRange(size_t begin, size_t end);

    void showTextBox();
    void hideTextBox();

    bool textBoxVisible() const {
        return mCaptionTextBox->visible();
    }

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
};

}
