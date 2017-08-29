// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/Common.h"

#include <nanogui/widget.h>

#include <string>

TEV_NAMESPACE_BEGIN

class ImageButton : public nanogui::Widget {
public:
    ImageButton(nanogui::Widget* parent, const std::string& caption, bool canBeReference);

    Eigen::Vector2i preferredSize(NVGcontext *ctx) const override;

    bool mouseButtonEvent(const Eigen::Vector2i &p, int button, bool down, int modifiers) override;

    void draw(NVGcontext *ctx) override;

    void setReferenceCallback(const std::function<void(bool)> &callback) {
        mReferenceCallback = callback;
    }

    void setIsReference(bool isReference) {
        mIsReference = isReference;
    }

    void setSelectedCallback(const std::function<void()> &callback) {
        mSelectedCallback = callback;
    }

    void setIsSelected(bool isSelected) {
        mIsSelected = isSelected;
    }

    void setId(size_t id) {
        mId = id;
    }

private:
    std::string mCaption;
    bool mCanBeReference;

    bool mIsReference = false;
    std::function<void(bool)> mReferenceCallback;

    bool mIsSelected = false;
    std::function<void()> mSelectedCallback;

    size_t mId = 0;
};

TEV_NAMESPACE_END
