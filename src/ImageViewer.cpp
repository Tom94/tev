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
#include <nanogui/tabwidget.h>
#include <nanogui/vscrollpanel.h>

#include <Iex.h>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageViewer::ImageViewer()
    : nanogui::Screen(Vector2i{1024, 767}, "tev") {

    auto screenSplit = new Widget(this);
    screenSplit->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

    auto leftSide = new Widget(screenSplit);
    leftSide->setFixedWidth(mMenuWidth);
    leftSide->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    mImageCanvas = new ImageCanvas{screenSplit, pixelRatio()};

    // Exposure label and slider
    {
        auto panel = new Widget{leftSide};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        auto label = new Label{panel, "Tonemapping", "sans-bold", 20};
        label->setTooltip(
            "The Exposure Value (EV) scales the brightness of an image prior to tonemapping by 2^EV. "
            "It can be controlled via the GUI, or by pressing E/Shift+E."
        );

        panel = new Widget{leftSide};
        panel->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5});

        mExposureLabel = new Label{panel, "", "sans-bold"};

        mExposureSlider = new Slider{panel};
        mExposureSlider->setRange({-5.0f, 5.0f});
        mExposureSlider->setFixedWidth(mMenuWidth - 47);

        mExposureSlider->setCallback([this](float value) {
            setExposure(value);
        });
        setExposure(0);
    }

    // Tonemap options
    {
        mTonemapButtonContainer = new Widget{leftSide};
        mTonemapButtonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5, 1});

        auto makeTonemapButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{mTonemapButtonContainer, name};
            button->setFlags(Button::RadioButton);
            button->setFontSize(15);
            button->setCallback(callback);
            return button;
        };

        auto errorButton = makeTonemapButton("sRGB", [this]() {
            setTonemap(ETonemap::SRGB);
        });

        makeTonemapButton("Gamma", [this]() {
            setTonemap(ETonemap::Gamma);
        });

        makeTonemapButton("False-color", [this]() {
            setTonemap(ETonemap::FalseColor);
        });

        errorButton->setPushed(true);
    }

    // Error metrics
    {
        mMetricButtonContainer = new Widget{leftSide};
        mMetricButtonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5, 2});

        auto makeMetricButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{mMetricButtonContainer, name};
            button->setFlags(Button::RadioButton);
            button->setFontSize(15);
            button->setCallback(callback);
            return button;
        };

        auto errorButton = makeMetricButton("E", [this]() {
            setMetric(EMetric::Error);
        });

        makeMetricButton("AE", [this]() {
            setMetric(EMetric::AbsoluteError);
        });

        makeMetricButton("SE", [this]() {
            setMetric(EMetric::SquaredError);
        });

        makeMetricButton("RAE", [this]() {
            setMetric(EMetric::RelativeAbsoluteError);
        });

        makeMetricButton("RSE", [this]() {
            setMetric(EMetric::RelativeSquaredError);
        });

        errorButton->setPushed(true);
    }

    // Image selection
    {
        auto spacer = new Widget{leftSide};
        spacer->setHeight(10);

        auto panel = new Widget{leftSide};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        auto label = new Label{panel, "Images", "sans-bold", 20};
        label->setTooltip(
            "Select images either by left-clicking on them or by pressing arrow/number keys on your keyboard.\n"
            "Right-clicking an image marks it as the 'reference' image. "
            "While a reference image is set, the currently selected image is not simply displayed, but compared to the reference image."
        );

        mImageScrollContainer = new VScrollPanel{leftSide};
        auto scrollContent = new Widget{mImageScrollContainer};
        scrollContent->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

        mImageButtonContainer = new Widget{scrollContent};
        mImageButtonContainer->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
        mImageScrollContainer->setFixedWidth(mMenuWidth);

        spacer = new Widget{scrollContent};
        spacer->setHeight(3);

        auto tools = new Widget{scrollContent};
        tools->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        auto b = new Button{tools, "Open image file"};
        b->setCallback([&] {
            string path = file_dialog(
            {
                {"exr",  "OpenEXR image"},
                {"hdr",  "HDR image"},
                {"bmp",  "Bitmap Image File"},
                {"gif",  "Graphics Interchange Format image"},
                {"jpg",  "JPEG image"},
                {"jpeg", "JPEG image"},
                {"pic",  "PIC image"},
                {"png",  "Portable Network Graphics image"},
                {"pnm",  "Portable Any Map image"},
                {"psd",  "PSD image"},
                {"tga",  "Truevision TGA image"},
            },
            false
            );

            if (!path.empty()) {
                tryLoadImage(path, true);
            }
        });
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
        tryLoadImage(imageFile, true);
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
            int idx = (key - GLFW_KEY_1 + 10) % 10;
            if (idx >= 0 && idx < amountImages) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    selectReference(idx);
                } else {
                    selectImage(idx);
                }
            }
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

        if (key == GLFW_KEY_UP || key == GLFW_KEY_W || key == GLFW_KEY_PAGE_UP) {
            if (modifiers & GLFW_MOD_SHIFT) {
                selectReference((mCurrentReference - 1 + amountImages) % amountImages);
            } else {
                selectImage((mCurrentImage - 1 + amountImages) % amountImages);
            }
        } else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S || key == GLFW_KEY_PAGE_DOWN) {
            if (modifiers & GLFW_MOD_SHIFT) {
                selectReference((mCurrentReference + 1) % amountImages);
            } else {
                selectImage((mCurrentImage + 1) % amountImages);
            }
        }

        if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() + 1) % AmountTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                setMetric(static_cast<EMetric>((metric() + 1) % AmountMetrics));
            }
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() - 1 + AmountTonemaps) % AmountTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                setMetric(static_cast<EMetric>((metric() - 1 + AmountMetrics) % AmountMetrics));
            }
        }
    }

    return false;
}

void ImageViewer::draw(NVGcontext *ctx) {
    Screen::draw(ctx);
}

void ImageViewer::addImage(shared_ptr<Image> image, bool shallSelect) {
    size_t index = mImageInfos.size();

    auto button = new ImageButton{mImageButtonContainer, image->name()};
    button->setFontSize(15);
    button->setId(index + 1);

    button->setSelectedCallback([this,index]() {
        selectImage(index);
    });
    button->setReferenceCallback([this, index](bool isReference) {
        if (!isReference) {
            unselectReference();
        } else {
            selectReference(index);
        }
    });

    mImageInfos.push_back({
        image,
        button,
    });

    performLayout();

    // First image got added, let's select it.
    if (index == 0 || shallSelect) {
        selectImage(index);
        fitAllImages();
    }
}

void ImageViewer::tryLoadImage(const std::string& filename, bool shallSelect) {
    try {
        addImage(make_shared<Image>(filename), shallSelect);
    } catch (invalid_argument e) {
        tfm::format(cerr, "Could not load image from %s: %s\n", filename, e.what());
    } catch (Iex::BaseExc& e) {
        tfm::format(cerr, "Could not load image from %s: %s\n", filename, e.what());
    }
}

void ImageViewer::selectImage(size_t index) {
    if (index >= mImageInfos.size()) {
        throw invalid_argument{tfm::format("Invalid image index (%d) should be in range [0,%d).", index, mImageInfos.size())};
    }

    for (size_t i = 0; i < mImageInfos.size(); ++i) {
        mImageInfos[i].button->setIsSelected(i == index);
    }

    mCurrentImage = index;
    mImageCanvas->setImage(mImageInfos[mCurrentImage].image);
}

void ImageViewer::unselectReference() {
    for (size_t i = 0; i < mImageInfos.size(); ++i) {
        mImageInfos[i].button->setIsReference(false);
    }

    mCurrentReference = 0;
    mImageCanvas->setReference(nullptr);
}

void ImageViewer::selectReference(size_t index) {
    if (index >= mImageInfos.size()) {
        throw invalid_argument{tfm::format("Invalid reference index (%d) should be in range [0,%d).", index, mImageInfos.size())};
    }

    for (size_t i = 0; i < mImageInfos.size(); ++i) {
        mImageInfos[i].button->setIsReference(i == index);
    }

    mCurrentReference = index;
    mImageCanvas->setReference(mImageInfos[mCurrentReference].image);
}

void ImageViewer::setExposure(float value) {
    value = round(value, 1.0f);
    mExposureSlider->setValue(value);
    mExposureLabel->setCaption(tfm::format("EV%+.1f", value));

    mImageCanvas->setExposure(value);
}

void ImageViewer::setTonemap(ETonemap tonemap) {
    mImageCanvas->setTonemap(tonemap);
    auto& buttons = mTonemapButtonContainer->children();
    for (int i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed(i == tonemap);
    }
}

void ImageViewer::setMetric(EMetric metric) {
    mImageCanvas->setMetric(metric);
    auto& buttons = mMetricButtonContainer->children();
    for (int i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed(i == metric);
    }
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

void ImageViewer::maximize() {
    glfwMaximizeWindow(mGLFWWindow);
}

TEV_NAMESPACE_END
