// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

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

    mVerticalScreenSplit = new Widget(this);
    mVerticalScreenSplit->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

    auto horizontalScreenSplit = new Widget(mVerticalScreenSplit);
    horizontalScreenSplit->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

    mSidebar = new Widget(horizontalScreenSplit);
    mSidebar->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});
    mSidebar->setFixedWidth(200);

    mImageCanvas = new ImageCanvas{horizontalScreenSplit, pixelRatio()};

    // Exposure label and slider
    {
        auto panel = new Widget{mSidebar};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        auto label = new Label{panel, "Tonemapping", "sans-bold", 25};
        label->setTooltip(
            "Various tonemapping options. Hover the labels of individual options to learn more!"
        );

        panel = new Widget{mSidebar};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        mExposureLabel = new Label{panel, "", "sans-bold", 15};
        mExposureLabel->setTooltip(
            "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure. "
            "It can be controlled via the GUI, or by pressing E/Shift+E."
        );

        mExposureSlider = new Slider{panel};
        mExposureSlider->setRange({-5.0f, 5.0f});
        mExposureSlider->setCallback([this](float value) {
            setExposure(value);
        });
        setExposure(0);

        panel = new Widget{mSidebar};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        mOffsetLabel = new Label{panel, "", "sans-bold", 15};
        mOffsetLabel->setTooltip(
            "The offset is added to the image after exposure has been applied. "
            "It can be controlled via the GUI, or by pressing O/Shift+O."
        );

        mOffsetSlider = new Slider{panel};
        mOffsetSlider->setRange({-1.0f, 1.0f});
        mOffsetSlider->setCallback([this](float value) {
            setOffset(value);
        });
        setOffset(0);
    }

    // Exposure/offset buttons
    {
        auto buttonContainer = new Widget{mSidebar};
        buttonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5, 2});

        auto makeButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{buttonContainer, name};
            button->setFontSize(15);
            button->setCallback(callback);
            return button;
        };

        makeButton("Reset", [this]() {
            resetExposureAndOffset();
        });

        makeButton("Normalize", [this]() {
            normalizeExposureAndOffset();
        });
    }

    // Tonemap options
    {
        mTonemapButtonContainer = new Widget{mSidebar};
        mTonemapButtonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5, 2});

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

        makeTonemapButton("FC", [this]() {
            setTonemap(ETonemap::FalseColor);
        });

        makeTonemapButton("+/-", [this]() {
            setTonemap(ETonemap::PositiveNegative);
        });

        errorButton->setPushed(true);
    }

    // Error metrics
    {
        mMetricButtonContainer = new Widget{mSidebar};
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
        auto spacer = new Widget{mSidebar};
        spacer->setHeight(10);

        auto panel = new Widget{mSidebar};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        auto label = new Label{panel, "Images", "sans-bold", 25};
        label->setTooltip(
            "Select images either by left-clicking on them or by pressing arrow/number keys on your keyboard.\n"
            "Right-clicking an image marks it as the 'reference' image. "
            "While a reference image is set, the currently selected image is not simply displayed, but compared to the reference image."
        );

        panel = new Widget{mSidebar};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        label = new Label{panel, "Filter", "sans-bold", 15};

        mFilter = new TextBox{panel, ""};
        mFilter->setEditable(true);
        mFilter->setAlignment(TextBox::Alignment::Left);
        mFilter->setCallback([this](const string& filter) {
            return setFilter(filter);
        });

        mImageScrollContainer = new VScrollPanel{mSidebar};
        mImageScrollContainer->setFixedWidth(mSidebar->fixedWidth());

        auto scrollContent = new Widget{mImageScrollContainer};
        scrollContent->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

        mImageButtonContainer = new Widget{scrollContent};
        mImageButtonContainer->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

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
                auto image = tryLoadImage(path);
                if (image) {
                    addImage(image, true);
                }

                // Make sure we gain focus after seleting a file to be loaded.
                glfwFocusWindow(mGLFWWindow);
            }
        });
    }

    // Layer selection
    {
        mFooter = new Widget{mVerticalScreenSplit};
        mFooter->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

        mLayerButtonContainer = new Widget{mFooter};
        mLayerButtonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});
        mFooter->setFixedHeight(25);
        mFooter->setVisible(false);
    }

    setResizeCallback([this](Vector2i) { updateLayout(); });

    this->setSize(Vector2i(1024, 768));
    selectReference(nullptr);
}

bool ImageViewer::dropEvent(const std::vector<std::string>& filenames) {
    if (Screen::dropEvent(filenames)) {
        return true;
    }

    for (const auto& imageFile : filenames) {
        auto image = tryLoadImage(imageFile);
        if (image) {
            addImage(image, true);
        }
    }

    // Make sure we gain focus after dragging files into here.
    glfwFocusWindow(mGLFWWindow);
    return true;
}

bool ImageViewer::keyboardEvent(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboardEvent(key, scancode, action, modifiers)) {
        return true;
    }

    int amountLayers = mLayerButtonContainer->childCount();

    if (action == GLFW_PRESS) {
        if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
            int idx = (key - GLFW_KEY_1 + 10) % 10;
            if (modifiers & GLFW_MOD_SHIFT) {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    if (mCurrentReference == image) {
                        selectReference(nullptr);
                    } else {
                        selectReference(image);
                    }
                }
            } else if (modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
                if (idx >= 0 && idx < amountLayers) {
                    selectLayer(nthVisibleLayer(idx));
                }
            } else {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    selectImage(image);
                }
            }
        } else if (key == GLFW_KEY_N) {
            normalizeExposureAndOffset();
        } else if (key == GLFW_KEY_R) {
            resetExposureAndOffset();
        } else if (key == GLFW_KEY_B) {
            if (modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
                setUiVisible(!isUiVisible());
            }
        } else if (key == GLFW_KEY_P) {
            if (modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
                mFilter->setValue("");
                mFilter->requestFocus();
            }
        } else if (key == GLFW_KEY_F) {
            if (mCurrentImage) {
                mImageCanvas->fitImageToScreen(*mCurrentImage);
            }
        } else if (key == GLFW_KEY_ENTER) {
            if (modifiers & GLFW_MOD_ALT) {
                toggleMaximized();
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

        if (key == GLFW_KEY_O) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setOffset(offset() - 0.1f);
            } else {
                setOffset(offset() + 0.1f);
            }
        }

        if (key == GLFW_KEY_UP || key == GLFW_KEY_W || key == GLFW_KEY_PAGE_UP) {
            if (modifiers & GLFW_MOD_SHIFT) {
                selectReference(previousImage(mCurrentReference));
            } else {
                selectImage(previousImage(mCurrentImage));
            }
        } else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S || key == GLFW_KEY_PAGE_DOWN) {
            if (modifiers & GLFW_MOD_SHIFT) {
                selectReference(previousImage(mCurrentReference));
            } else {
                selectImage(nextImage(mCurrentImage));
            }
        }

        if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() + 1) % AmountTonemaps));
            } else if (modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() + 1) % AmountMetrics));
                }
            } else {
                selectLayer(nextLayer(mCurrentLayer));
            }
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() - 1 + AmountTonemaps) % AmountTonemaps));
            } else if (modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() - 1 + AmountMetrics) % AmountMetrics));
                }
            } else {
                selectLayer(previousLayer(mCurrentLayer));
            }
        }
    }

    return false;
}

void ImageViewer::drawContents() {
    updateTitle();
}

void ImageViewer::addImage(shared_ptr<Image> image, bool shallSelect) {
    if (!image) {
        throw invalid_argument{"Image may not be null."};
    }

    size_t index = mImages.size();

    auto button = new ImageButton{mImageButtonContainer, image->name(), true};
    button->setFontSize(15);
    button->setId(index + 1);
    button->setTooltip(image->toString());

    button->setSelectedCallback([this, image]() {
        selectImage(image);
    });

    button->setReferenceCallback([this, image](bool isReference) {
        if (!isReference) {
            selectReference(nullptr);
        } else {
            selectReference(image);
        }
    });

    mImages.push_back(image);

    // The following call will show the footer if there is not an image
    // with more than 1 layer.
    setUiVisible(isUiVisible());

    // Ensure the new image button will have the correct visibility state.
    setFilter(mFilter->value());

    updateLayout();

    // First image got added, let's select it.
    if (index == 0 || shallSelect) {
        selectImage(image);
        fitAllImages();
    }
}

void ImageViewer::selectImage(const shared_ptr<Image>& image) {
    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(false);
        }

        mCurrentImage = nullptr;
        mImageCanvas->setImage(nullptr);
        return;
    }

    size_t id = imageId(image);

    // Don't do anything if the image that wants to be selected is not visible.
    if (!mImageButtonContainer->childAt(id)->visible()) {
        return;
    }

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentImage = image;
    mImageCanvas->setImage(mCurrentImage);

    // Clear layer buttons
    while (mLayerButtonContainer->childCount() > 0) {
        mLayerButtonContainer->removeChild(mLayerButtonContainer->childCount() - 1);
    }

    size_t numLayers = mCurrentImage->layers().size();
    for (size_t i = 0; i < numLayers; ++i) {
        string layer = layerName(i);
        auto button = new ImageButton{mLayerButtonContainer, layer.empty() ? "<root>"s : layer, false};
        button->setFontSize(15);
        button->setId(i + 1);

        button->setSelectedCallback([this, layer]() {
            selectLayer(layer);
        });
    }

    // This will automatically fall back to the root layer if the current
    // layer isn't found.
    selectLayer(mCurrentLayer);

    // Setting the filter again makes sure, that layers are correctly filtered.
    setFilter(mFilter->value());
    updateLayout();
}

void ImageViewer::selectLayer(string layer) {
    size_t id = layerId(layer);

    auto& buttons = mLayerButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentLayer = layerName(id);
    mImageCanvas->setRequestedLayer(mCurrentLayer);
}

void ImageViewer::selectReference(const shared_ptr<Image>& image) {
    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsReference(false);
        }

        auto& metricButtons = mMetricButtonContainer->children();
        for (size_t i = 0; i < metricButtons.size(); ++i) {
            dynamic_cast<Button*>(metricButtons[i])->setEnabled(false);
        }

        mCurrentReference = nullptr;
        mImageCanvas->setReference(nullptr);
        return;
    }

    size_t id = imageId(image);

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsReference(i == id);
    }

    auto& metricButtons = mMetricButtonContainer->children();
    for (size_t i = 0; i < metricButtons.size(); ++i) {
        dynamic_cast<Button*>(metricButtons[i])->setEnabled(true);
    }

    mCurrentReference = image;
    mImageCanvas->setReference(mCurrentReference);
}

void ImageViewer::setExposure(float value) {
    value = round(value, 1.0f);
    mExposureSlider->setValue(value);
    mExposureLabel->setCaption(tfm::format("Exposure: %+.1f", value));

    mImageCanvas->setExposure(value);
}

void ImageViewer::setOffset(float value) {
    value = round(value, 2.0f);
    mOffsetSlider->setValue(value);
    mOffsetLabel->setCaption(tfm::format("Offset: %+.2f", value));

    mImageCanvas->setOffset(value);
}

void ImageViewer::normalizeExposureAndOffset() {
    if (!mCurrentImage) {
        return;
    }

    auto channels = mImageCanvas->getChannels(*mCurrentImage);

    float minimum = numeric_limits<float>::max();
    float maximum = numeric_limits<float>::min();
    for (const auto& channelName : channels) {
        const auto& channel = mCurrentImage->channel(channelName);

        for (size_t i = 0; i < channel->count(); ++i) {
            float val = channel->eval(i);
            if (val > maximum) {
                maximum = val;
            }
            if (val < minimum) {
                minimum = val;
            }
        }
    }

    float factor = 1.0f / (maximum - minimum);
    setExposure(log2(factor));
    setOffset(-minimum * factor);
}

void ImageViewer::resetExposureAndOffset() {
    setExposure(0);
    setOffset(0);
}

void ImageViewer::setTonemap(ETonemap tonemap) {
    mImageCanvas->setTonemap(tonemap);
    auto& buttons = mTonemapButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed(i == tonemap);
    }
}

void ImageViewer::setMetric(EMetric metric) {
    mImageCanvas->setMetric(metric);
    auto& buttons = mMetricButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed(i == metric);
    }
}

void ImageViewer::fitAllImages() {
    if (mImages.empty()) {
        return;
    }

    Vector2i maxSize = Vector2i::Zero();
    for (const auto& image : mImages) {
        maxSize = maxSize.cwiseMax(image->size());
    }

    // Convert from image pixel coordinates to nanogui coordinates.
    maxSize = (maxSize.cast<float>() / pixelRatio()).cast<int>();
    // Take into account the size of the menu on the left.
    maxSize.x() += mSidebar->width();

    // Only increase our current size if we are larger than the default size of the window.
    setSize(mSize.cwiseMax(maxSize));
}

bool ImageViewer::setFilter(const string& filter) {
    mFilter->setValue(filter);

    string imagePart = filter;
    string layerPart = "";

    auto shaPos = filter.find_last_of('#');
    if (shaPos != string::npos) {
        imagePart = filter.substr(0, shaPos);
        layerPart = filter.substr(shaPos + 1);
    }

    // Image filtering
    {
        const auto& buttons = mImageButtonContainer->children();
        for (Widget* button : buttons) {
            ImageButton* ib = dynamic_cast<ImageButton*>(button);
            ib->setVisible(matches(ib->caption(), imagePart));
        }

        if (mCurrentImage && !matches(mCurrentImage->name(), imagePart)) {
            selectImage(nthVisibleImage(0));
        }

        if (mCurrentReference && !matches(mCurrentReference->name(), imagePart)) {
            selectReference(nullptr);
        }
    }

    // Layer filtering
    if (mCurrentImage)
    {
        const auto& buttons = mLayerButtonContainer->children();
        for (Widget* button : buttons) {
            ImageButton* ib = dynamic_cast<ImageButton*>(button);
            ib->setVisible(matches(ib->caption(), layerPart));
        }

        if (!matches(mCurrentLayer, layerPart)) {
            selectLayer(nthVisibleLayer(0));
        }
    }

    updateLayout();
    return true;
}

void ImageViewer::maximize() {
    glfwMaximizeWindow(mGLFWWindow);
}

bool ImageViewer::isMaximized() {
    return glfwGetWindowAttrib(mGLFWWindow, GLFW_MAXIMIZED) != 0;
}

void ImageViewer::toggleMaximized() {
    if (isMaximized()) {
        glfwRestoreWindow(mGLFWWindow);
    } else {
        maximize();
    }
}

void ImageViewer::setUiVisible(bool shouldBeVisible) {
    mSidebar->setVisible(shouldBeVisible);

    bool shouldFooterBeVisible = false;
    for (const auto& image : mImages) {
        // There is no point showing the footer as long as no image
        // has more than the root layer.
        if (image->layers().size() > 1) {
            shouldFooterBeVisible = true;
            break;
        }
    }

    mFooter->setVisible(shouldFooterBeVisible && shouldBeVisible);

    updateLayout();
}

void ImageViewer::updateLayout() {
    float sidebarWidth = mSidebar->visible() ? mSidebar->fixedWidth() : 0;
    float footerHeight = mFooter->visible() ? mFooter->fixedHeight() : 0;
    mImageCanvas->setFixedSize(mSize - Vector2i{sidebarWidth, footerHeight});

    mVerticalScreenSplit->setFixedSize(mSize);
    mImageScrollContainer->setFixedHeight(
        mSize.y() - mImageScrollContainer->position().y() - footerHeight
    );

    performLayout();
}

void ImageViewer::updateTitle() {
    string caption = "tev";
    if (mCurrentImage) {
        auto channels = mImageCanvas->getChannels(*mCurrentImage);
        // Remove duplicates
        channels.erase(unique(begin(channels), end(channels)), end(channels));
        transform(begin(channels), end(channels), begin(channels), Channel::tail);

        string channelsString = join(channels, ",");
        caption = mCurrentImage->shortName();

        if (mCurrentLayer.empty()) {
            caption += " - "s + channelsString;
        } else {
            caption += " - "s + mCurrentLayer;
            if (channels.size() == 1) {
                caption += "."s + channelsString;
            } else {
                caption += ".("s + channelsString + ")"s;
            }
        }

        vector<float> values = mImageCanvas->getValues(mousePos());
        Vector2i imageCoords = mImageCanvas->getImageCoords(*mCurrentImage, mousePos());
        TEV_ASSERT(values.size() >= channels.size(), "Should obtain a value for every existing channel.");

        string valuesString;
        for (size_t i = 0; i < channels.size(); ++i) {
            valuesString += tfm::format("%.2f,", values[i]);
        }
        valuesString.pop_back();

        caption += " - "s + tfm::format("@(%d,%d)%s", imageCoords.x(), imageCoords.y(), valuesString);
    }

    setCaption(caption);
}

string ImageViewer::layerName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    return mCurrentImage->layers().at(index);
}

size_t ImageViewer::layerId(const string& layer) const {
    if (!mCurrentImage) {
        return 0;
    }

    const auto& layers = mCurrentImage->layers();
    auto pos = static_cast<size_t>(distance(begin(layers), find(begin(layers), end(layers), layer)));
    return pos >= layers.size() ? 0 : pos;
}

size_t ImageViewer::imageId(const shared_ptr<Image>& image) const {
    auto pos = static_cast<size_t>(distance(begin(mImages), find(begin(mImages), end(mImages), image)));
    return pos >= mImages.size() ? 0 : pos;
}

string ImageViewer::nextLayer(const string& layer) {
    int startId = (int)layerId(layer);

    int id = startId;
    do {
        id = (id + 1) % mLayerButtonContainer->childCount();
    } while (!mLayerButtonContainer->childAt(id)->visible() && id != startId);

    return layerName(id);
}

string ImageViewer::previousLayer(const string& layer) {
    int startId = (int)layerId(layer);

    int id = startId;
    do {
        id = (id + mLayerButtonContainer->childCount() - 1) % mLayerButtonContainer->childCount();
    } while (!mLayerButtonContainer->childAt(id)->visible() && id != startId);

    return layerName(id);
}

string ImageViewer::nthVisibleLayer(size_t n) {
    for (int i = 0; i < mLayerButtonContainer->childCount(); ++i) {
        if (mLayerButtonContainer->childAt(i)->visible()) {
            if (n == 0) {
                return layerName(i);
            }
            --n;
        }
    }
    return mCurrentLayer;
}

const shared_ptr<Image>& ImageViewer::nextImage(const shared_ptr<Image>& image) {
    int startId = (int)imageId(image);

    int id = startId;
    do {
        id = (id + 1) % mImages.size();
    } while (!mImageButtonContainer->childAt(id)->visible() && id != startId);

    return mImages[id];
}

const shared_ptr<Image>& ImageViewer::previousImage(const shared_ptr<Image>& image) {
    int startId = (int)imageId(image);

    int id = startId;
    do {
        id = (id + (int)mImages.size() - 1) % mImages.size();
    } while (!mImageButtonContainer->childAt(id)->visible() && id != startId);

    return mImages[id];
}

shared_ptr<Image> ImageViewer::nthVisibleImage(size_t n) {
    for (size_t i = 0; i < mImages.size(); ++i) {
        if (mImageButtonContainer->children()[i]->visible()) {
            if (n == 0) {
                return mImages[i];
            }
            --n;
        }
    }
    return nullptr;
}

TEV_NAMESPACE_END
