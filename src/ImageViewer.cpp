// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/ImageViewer.h"

#include <iostream>
#include <stdexcept>

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/layout.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/textbox.h>
#include <nanogui/slider.h>
#include <nanogui/tabwidget.h>
#include <nanogui/vscrollpanel.h>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageViewer::ImageViewer()
    : nanogui::Screen(Vector2i(1024, 767), "Viewer") {

    auto screenSplit = new Widget(this);
    screenSplit->setLayout(new BoxLayout(Orientation::Horizontal, Alignment::Fill));

    auto leftSide = new Widget(screenSplit);
    leftSide->setFixedWidth(mMenuWidth);
    leftSide->setLayout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

    mImageCanvas = new ImageCanvas(screenSplit, pixelRatio());

    performLayout();

    auto tools = new Widget(leftSide);
    tools->setLayout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 5));
    auto b = new Button(tools, "Open file");
    b->setCallback([&] {
        string path = file_dialog(
            {
                { "exr",  "OpenEXR image" },
                { "hdr",  "HDR image" },
                { "bmp",  "Bitmap Image File" },
                { "gif",  "Graphics Interchange Format image" },
                { "jpg",  "JPEG image" },
                { "jpeg", "JPEG image" },
                { "pic",  "PIC image" },
                { "png",  "Portable Network Graphics image" },
                { "pnm",  "Portable Any Map image" },
                { "psd",  "PSD image" },
                { "tga",  "Truevision TGA image" },
            },
            false
        );
        
        if (!path.empty()) {
            addImage(std::make_shared<Image>(path));
        }
    });

    // Exposure label and slider
    {
        auto spacer = new Widget(leftSide);
        spacer->setHeight(10);

        Widget* panel = new Widget(leftSide);
        panel->setLayout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 5));
        new Label(panel, "Settings", "sans-bold", 20);

        panel = new Widget(leftSide);
        panel->setLayout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 5));

        mExposureLabel = new Label(panel, "", "sans-bold");

        mExposureSlider = new Slider(panel);
        mExposureSlider->setRange({-5.0f, 5.0f});
        mExposureSlider->setFixedWidth(mMenuWidth - 50);
    }

    // Image selection
    {
        auto spacer = new Widget(leftSide);
        spacer->setHeight(10);

        auto panel = new Widget(leftSide);
        panel->setLayout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 5));
        new Label(panel, "Images", "sans-bold", 20);

        mImageScrollContainer = new VScrollPanel(leftSide);
        mImageButtonContainer = new Widget(mImageScrollContainer);
        mImageButtonContainer->setLayout(new BoxLayout(Orientation::Vertical, Alignment::Fill));
        mImageButtonContainer->setFixedWidth(mMenuWidth - 15);

        mExposureSlider->setCallback([this](float value) {
            setExposure(value);
        });
        setExposure(0);
    }

    setResizeCallback([this, screenSplit](Vector2i size) {
        screenSplit->setFixedSize(size);
        mImageCanvas->setFixedSize(size - Vector2i{mMenuWidth, 0});
        mImageScrollContainer->setFixedHeight(size.y() - mImageScrollContainer->position().y());

        performLayout();
    });

    this->setSize(Vector2i(1024, 768));
}

bool ImageViewer::dropEvent(const std::vector<std::string>& filenames) {
    if (Screen::dropEvent(filenames)) {
        return true;
    }

    for (const auto& imageFile : filenames) {
        addImage(make_shared<Image>(imageFile));
    }

    return true;
}

bool ImageViewer::keyboardEvent(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboardEvent(key, scancode, action, modifiers)) {
        return true;
    }

    int amountImages = static_cast<int>(mImageInfos.size());

    if (action == GLFW_PRESS) {
        if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
            selectImage(clamp(key - GLFW_KEY_1, 0, amountImages - 1));
        } else if (key == GLFW_KEY_LEFT) {
            selectImage((mCurrentImage - 1 + amountImages) % amountImages);
        } else if (key == GLFW_KEY_RIGHT) {
            selectImage((mCurrentImage + 1 + amountImages) % amountImages);
        } else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) {
            setVisible(false);
            return true;
        }
    }

    // Hotkeys for changing values like exposure should also respond to repeats
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_E) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setExposure(exposure() - 0.5f);
            } else {
                setExposure(exposure() + 0.5f);
            }
        }
    }

    return false;
}

void ImageViewer::draw(NVGcontext *ctx) {
    Screen::draw(ctx);
}

void ImageViewer::addImage(shared_ptr<Image> image) {
    size_t index = mImageInfos.size();

    auto button = new ImageButton(nvgContext(), mImageButtonContainer, image->name());
    button->setCallback([this,index]() {
        selectImage(index);
    });
    button->setFlags(Button::RadioButton);
    button->setFontSize(15);

    mImageInfos.push_back({
        image,
        button,
    });

    performLayout();

    // First image got added, let's select it.
    if (index == 0) {
        selectImage(index);
    }
}

void ImageViewer::selectImage(size_t index) {
    if (index >= mImageInfos.size()) {
        throw invalid_argument("Invalid image index.");
    }

    for (size_t i = 0; i < mImageInfos.size(); ++i) {
        mImageInfos[i].button->setPushed(i == index);
    }

    mCurrentImage = index;
    mImageCanvas->setImage(mImageInfos[mCurrentImage].image);
}

float ImageViewer::exposure() {
    return mExposureSlider->value();
}

void ImageViewer::setExposure(float value) {
    value = round(value * 10) / 10;
    mExposureSlider->setValue(value);
    mExposureLabel->setCaption(format("EV%+.1f", value));

    mImageCanvas->setExposure(value);
}

void ImageViewer::fitAllImages() {
    if (mImageInfos.empty()) {
        return;
    }

    Vector2i maxSize = Vector2i::Zero();
    for (const auto& imageInfo : mImageInfos) {
        maxSize = maxSize.cwiseMax(imageInfo.image->size());
    }

    // Convert from image pixel coordinates to nanogui coordinates.
    maxSize = (maxSize.cast<float>() / pixelRatio()).cast<int>();
    // Take into account the size of the menu on the left.
    maxSize.x() += mMenuWidth;

    // Only increase our current size if we are larger than the default size of the window.
    setSize(mSize.cwiseMax(maxSize));
}

TEV_NAMESPACE_END
