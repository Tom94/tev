// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ImageViewer.h>
#include <tev/ThreadPool.h>

#include <iostream>
#include <stdexcept>

#include <nanogui/button.h>
#include <nanogui/entypo.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/vscrollpanel.h>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageViewer::ImageViewer(shared_ptr<Ipc> ipc, shared_ptr<SharedQueue<ImageAddition>> imagesToAdd, bool processPendingDrops)
: nanogui::Screen{Vector2i{1024, 799}, "tev"}, mIpc{ipc}, mImagesToAdd{imagesToAdd} {
    // At this point we no longer need the standalone console (if it exists).
    toggleConsole();

    TEV_ASSERT(mIpc->isPrimaryInstance(), "ImageViewer may only exist in the primary instance of tev.");

    mBackground = Color{0.23f, 1.0f};

    mVerticalScreenSplit = new Widget{this};
    mVerticalScreenSplit->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

    auto horizontalScreenSplit = new Widget(mVerticalScreenSplit);
    horizontalScreenSplit->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

    mSidebar = new Widget{horizontalScreenSplit};
    mSidebar->setFixedWidth(205);

    mHelpButton = new Button{mSidebar, "", ENTYPO_ICON_HELP};
    mHelpButton->setCallback([this]() { toggleHelpWindow(); });
    mHelpButton->setFontSize(15);
    mHelpButton->setTooltip("Information about using tev.");

    mSidebarLayout = new Widget{mSidebar};
    mSidebarLayout->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    mImageCanvas = new ImageCanvas{horizontalScreenSplit, pixelRatio()};

    // Exposure label and slider
    {
        auto panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});
        new Label{panel, "Tonemapping", "sans-bold", 25};
        panel->setTooltip(
            "Various tonemapping options. Hover the individual controls to learn more!"
        );

        panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        mExposureLabel = new Label{panel, "", "sans-bold", 15};

        mExposureSlider = new Slider{panel};
        mExposureSlider->setRange({-5.0f, 5.0f});
        mExposureSlider->setCallback([this](float value) {
            setExposure(value);
        });
        setExposure(0);

        panel->setTooltip(
            "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure.\n\n"
            "Keyboard shortcuts:\nE and Shift+E"
        );

        panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        mOffsetLabel = new Label{panel, "", "sans-bold", 15};

        mOffsetSlider = new Slider{panel};
        mOffsetSlider->setRange({-1.0f, 1.0f});
        mOffsetSlider->setCallback([this](float value) {
            setOffset(value);
        });
        setOffset(0);

        panel->setTooltip(
            "The offset is added to the image after exposure has been applied.\n\n"
            "Keyboard shortcuts:\nO and Shift+O"
        );
    }

    // Exposure/offset buttons
    {
        auto buttonContainer = new Widget{mSidebarLayout};
        buttonContainer->setLayout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

        auto makeButton = [&](const string& name, function<void()> callback, int icon = 0, string tooltip = "") {
            auto button = new Button{buttonContainer, name, icon};
            button->setFontSize(15);
            button->setCallback(callback);
            button->setTooltip(tooltip);
            return button;
        };

        mCurrentImageButtons.push_back(
            makeButton("Normalize", [this]() { normalizeExposureAndOffset(); }, 0, "Shortcut: N")
        );
        makeButton("Reset", [this]() { resetImage(); }, 0, "Shortcut: R");
    }

    // Tonemap options
    {
        mTonemapButtonContainer = new Widget{mSidebarLayout};
        mTonemapButtonContainer->setLayout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

        auto makeTonemapButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{mTonemapButtonContainer, name};
            button->setFlags(Button::RadioButton);
            button->setFontSize(15);
            button->setCallback(callback);
            return button;
        };

        makeTonemapButton("sRGB",  [this]() { setTonemap(ETonemap::SRGB); });
        makeTonemapButton("Gamma", [this]() { setTonemap(ETonemap::Gamma); });
        makeTonemapButton("FC",    [this]() { setTonemap(ETonemap::FalseColor); });
        makeTonemapButton("+/-",   [this]() { setTonemap(ETonemap::PositiveNegative); });

        setTonemap(ETonemap::SRGB);

        mTonemapButtonContainer->setTooltip(
            "Tonemap operator selection:\n\n"

            "sRGB\n"
            "Linear to sRGB conversion\n\n"

            "Gamma\n"
            "Inverse power gamma correction\n\n"

            "FC\n"
            "False-color visualization\n\n"

            "+/-\n"
            "Positive=Green, Negative=Red"
        );
    }

    // Error metrics
    {
        mMetricButtonContainer = new Widget{mSidebarLayout};
        mMetricButtonContainer->setLayout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

        auto makeMetricButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{mMetricButtonContainer, name};
            button->setFlags(Button::RadioButton);
            button->setFontSize(15);
            button->setCallback(callback);
            return button;
        };

        makeMetricButton("E",   [this]() { setMetric(EMetric::Error); });
        makeMetricButton("AE",  [this]() { setMetric(EMetric::AbsoluteError); });
        makeMetricButton("SE",  [this]() { setMetric(EMetric::SquaredError); });
        makeMetricButton("RAE", [this]() { setMetric(EMetric::RelativeAbsoluteError); });
        makeMetricButton("RSE", [this]() { setMetric(EMetric::RelativeSquaredError); });

        setMetric(EMetric::AbsoluteError);

        mMetricButtonContainer->setTooltip(
            "Error metric selection. Given a reference image r and the selected image i, "
            "the following operators are available:\n\n"

            "E (Error)\n"
            "i - r\n\n"

            "AE (Absolute Error)\n"
            "|i - r|\n\n"

            "SE (Squared Error)\n"
            "(i - r)²\n\n"

            "RAE (Relative Absolute Error)\n"
            "|i - r| / (r + 0.01)\n\n"

            "RSE (Relative Squared Error)\n"
            "(i - r)² / (r² + 0.01)"
        );
    }

    // Image selection
    {
        auto spacer = new Widget{mSidebarLayout};
        spacer->setHeight(10);

        auto panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        auto label = new Label{panel, "Images", "sans-bold", 25};
        label->setTooltip(
            "Select images either by left-clicking on them or by pressing arrow/number keys on your keyboard.\n"
            "Right-clicking an image marks it as the 'reference' image. "
            "While a reference image is set, the currently selected image is not simply displayed, but compared to the reference image."
        );

        panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        mHistogram = new MultiGraph{panel, ""};

        panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        mFilter = new TextBox{panel, ""};
        mFilter->setEditable(true);
        mFilter->setAlignment(TextBox::Alignment::Left);
        mFilter->setCallback([this](const string& filter) {
            return setFilter(filter);
        });

        mFilter->setPlaceholder("Enter text to filter images");
        mFilter->setTooltip(tfm::format(
            "Filters visible images and layers according to a supplied string. "
            "The string must have the format 'image:layer'. "
            "Only images whose name contains 'image' and layers whose name contains 'layer' will be visible.\n\n"
            "Keyboard shortcut:\n%s+P",
            HelpWindow::COMMAND
        ));

        auto tools = new Widget{mSidebarLayout};
        tools->setLayout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 1});

        auto makeImageButton = [&](const string& name, bool enabled, function<void()> callback, int icon = 0, string tooltip = "") {
            auto button = new Button{tools, name, icon};
            button->setCallback(callback);
            button->setTooltip(tooltip);
            button->setFontSize(15);
            button->setEnabled(enabled);
            return button;
        };

        makeImageButton("", true, [this] {
            openImageDialog();
        }, ENTYPO_ICON_FOLDER, tfm::format("Open (%s+O)", HelpWindow::COMMAND));

        mCurrentImageButtons.push_back(makeImageButton("", false, [this] {
            saveImageDialog();
        }, ENTYPO_ICON_SAVE, tfm::format("Save (%s+S)", HelpWindow::COMMAND)));

        mCurrentImageButtons.push_back(makeImageButton("", false, [this] {
            reloadImage(mCurrentImage);
        }, ENTYPO_ICON_CYCLE, tfm::format("Reload (%s+R or F5)", HelpWindow::COMMAND)));

        mAnyImageButtons.push_back(makeImageButton("A", false, [this] {
            reloadAllImages();
        }, ENTYPO_ICON_CYCLE, tfm::format("Reload All (%s+Shift+R or %s+F5)", HelpWindow::COMMAND, HelpWindow::COMMAND)));

        mCurrentImageButtons.push_back(makeImageButton("", false, [this] {
            removeImage(mCurrentImage);
        }, ENTYPO_ICON_CIRCLED_CROSS, tfm::format("Close (%s+W)", HelpWindow::COMMAND)));

        spacer = new Widget{mSidebarLayout};
        spacer->setHeight(3);

        mImageScrollContainer = new VScrollPanel{mSidebarLayout};
        mImageScrollContainer->setFixedWidth(mSidebarLayout->fixedWidth());

        mScrollContent = new Widget{mImageScrollContainer};
        mScrollContent->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

        mImageButtonContainer = new Widget{mScrollContent};
        mImageButtonContainer->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
    }

    // Layer selection
    {
        mFooter = new Widget{mVerticalScreenSplit};

        mLayerButtonContainer = new Widget{mFooter};
        mLayerButtonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});
        mLayerButtonContainer->setFixedHeight(25);
        mFooter->setFixedHeight(25);
        mFooter->setVisible(false);
    }

    setResizeCallback([this](Vector2i) { requestLayoutUpdate(); });

    this->setSize(Vector2i(1024, 800));
    selectImage(nullptr);
    selectReference(nullptr);

    if (processPendingDrops) {
        dropEvent(mPendingDrops);
    }
}

bool ImageViewer::mouseButtonEvent(const Vector2i &p, int button, bool down, int modifiers) {
    if (Screen::mouseButtonEvent(p, button, down, modifiers)) {
        return true;
    }

    if (down) {
        if (canDragSidebarFrom(p)) {
            mIsDraggingSidebar = true;
            return true;
        } else if (mImageCanvas->contains(p)) {
            mIsDraggingImage = true;
            return true;
        }
    } else {
        mIsDraggingSidebar = false;
        mIsDraggingImage = false;
    }

    return false;
}

bool ImageViewer::mouseMotionEvent(const Eigen::Vector2i& p, const Eigen::Vector2i& rel, int button, int modifiers) {
    if (Screen::mouseMotionEvent(p, rel, button, modifiers)) {
        return true;
    }

    if (mIsDraggingSidebar || canDragSidebarFrom(p)) {
        mSidebarLayout->setCursor(Cursor::HResize);
        mImageCanvas->setCursor(Cursor::HResize);
    } else {
        mSidebarLayout->setCursor(Cursor::Arrow);
        mImageCanvas->setCursor(Cursor::Arrow);
    }

    if (mIsDraggingSidebar) {
        mSidebar->setFixedWidth(clamp(p.x(), 205, mSize.x() - 10));
        requestLayoutUpdate();
    } else if (mIsDraggingImage) {
        // If left mouse button is held, move the image with mouse movement
        if ((button & 1) != 0) {
            mImageCanvas->translate(rel.cast<float>());
        }

        // If middle mouse button is held, zoom in-out with up-down mouse movement
        if ((button & 4) != 0) {
            mImageCanvas->scale(rel.y() / 10.0f, p.cast<float>());
        }
    }

    return false;
}

bool ImageViewer::dropEvent(const vector<string>& filenames) {
    if (Screen::dropEvent(filenames)) {
        return true;
    }

    for (size_t i = 0; i < filenames.size(); ++i) {
        const string& imageFile = filenames[i];
        bool shallSelect = i == filenames.size() - 1;
        ThreadPool::singleWorker().enqueueTask([imageFile, shallSelect, this] {
            auto image = tryLoadImage(imageFile, "");
            if (image) {
                mImagesToAdd->push({shallSelect, image});
            }
        });
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

    // Keybindings which should _not_ respond to repeats
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
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (idx >= 0 && idx < amountLayers) {
                    selectLayer(nthVisibleLayer(idx));
                }
            } else {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    selectImage(image);
                }
            }
            return true;
        } else if (key == GLFW_KEY_N) {
            normalizeExposureAndOffset();
            return true;
        } else if (key == GLFW_KEY_R) {
            if (modifiers & SYSTEM_COMMAND_MOD) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    reloadAllImages();
                } else {
                    reloadImage(mCurrentImage);
                }
            } else {
                resetImage();
            }
            return true;
        } else if (key == GLFW_KEY_B && modifiers & SYSTEM_COMMAND_MOD) {
            setUiVisible(!isUiVisible());
        } else if (key == GLFW_KEY_O && modifiers & SYSTEM_COMMAND_MOD) {
            openImageDialog();
            return true;
        } else if (key == GLFW_KEY_S && modifiers & SYSTEM_COMMAND_MOD) {
            saveImageDialog();
            return true;
        } else if (key == GLFW_KEY_P && modifiers & SYSTEM_COMMAND_MOD) {
            mFilter->requestFocus();
            return true;
        } else if (key == GLFW_KEY_F) {
            if (mCurrentImage) {
                mImageCanvas->fitImageToScreen(*mCurrentImage);
            }
            return true;
        } else if (key == GLFW_KEY_H) {
            toggleHelpWindow();
            return true;
        } else if (key == GLFW_KEY_ENTER && modifiers & GLFW_MOD_ALT) {
            toggleMaximized();
            return true;
        } else if (key == GLFW_KEY_F5) {
            if (modifiers & SYSTEM_COMMAND_MOD) {
                reloadAllImages();
            } else {
                reloadImage(mCurrentImage);
            }
            return true;
        } else if (key == GLFW_KEY_F12) {
            // For debugging purposes.
            toggleConsole();
            return true;
        } else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) {
            setVisible(false);
            return true;
        }
    }

    // Keybindings which should respond to repeats
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

        if (key == GLFW_KEY_W && modifiers & SYSTEM_COMMAND_MOD) {
            removeImage(mCurrentImage);
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_W || key == GLFW_KEY_PAGE_UP) {
            if (modifiers & GLFW_MOD_SHIFT) {
                selectReference(nextImage(mCurrentReference, Backward));
            } else {
                selectImage(nextImage(mCurrentImage, Backward));
            }
        } else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S || key == GLFW_KEY_PAGE_DOWN) {
            if (modifiers & GLFW_MOD_SHIFT) {
                selectReference(nextImage(mCurrentReference, Forward));
            } else {
                selectImage(nextImage(mCurrentImage, Forward));
            }
        }

        if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() + 1) % AmountTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() + 1) % AmountMetrics));
                }
            } else {
                selectLayer(nextLayer(mCurrentLayer, Forward));
            }
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() - 1 + AmountTonemaps) % AmountTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() - 1 + AmountMetrics) % AmountMetrics));
                }
            } else {
                selectLayer(nextLayer(mCurrentLayer, Backward));
            }
        }
    }

    return false;
}

void ImageViewer::drawContents() {
    bool receivedFileViaIpc = false;
    while (mIpc->receiveFromSecondaryInstance([this](const string& imageString) {
        ThreadPool::singleWorker().enqueueTask([imageString, this] {
            size_t colonPos = min(imageString.length() - 1, imageString.find_last_of(":"));
            auto image = tryLoadImage(imageString.substr(0, colonPos), imageString.substr(colonPos + 1));
            if (image) {
                mImagesToAdd->push({true, image});
            }
        });
    })) {
        receivedFileViaIpc = true;
    }

    try {
        while (true) {
            auto addition = mImagesToAdd->tryPop();
            addImage(addition.image, addition.shallSelect);
        }
    } catch (runtime_error) {
    }

    if (receivedFileViaIpc) {
        glfwFocusWindow(mGLFWWindow);
    }

    if (mRequiresFilterUpdate) {
        updateFilter();
        mRequiresFilterUpdate = false;
    }

    if (mRequiresLayoutUpdate) {
        updateLayout();
        mRequiresLayoutUpdate = false;
    }

    updateTitle();

    // Update histogram
    static const string histogramTooltipBase = "Histogram of color values. Adapts to the currently chosen layer and error metric.";
    auto lazyCanvasStatistics = mImageCanvas->canvasStatistics();
    if (lazyCanvasStatistics) {
        if (lazyCanvasStatistics->isReady()) {
            auto statistics = lazyCanvasStatistics->get();
            mHistogram->setValues(statistics->histogram);
            mHistogram->setMinimum(statistics->minimum);
            mHistogram->setMean(statistics->mean);
            mHistogram->setMaximum(statistics->maximum);
            mHistogram->setZero(statistics->histogramZero);
            mHistogram->setTooltip(tfm::format(
                "%s\n\n"
                "Minimum: %.3f\n"
                "Mean: %.3f\n"
                "Maximum: %.3f",
                histogramTooltipBase,
                statistics->minimum,
                statistics->mean,
                statistics->maximum)
            );
        }

    } else {
        mHistogram->setValues(MatrixXf::Zero(1, 1));
        mHistogram->setMinimum(0);
        mHistogram->setMean(0);
        mHistogram->setMaximum(0);
        mHistogram->setZero(0);
        mHistogram->setTooltip(
            tfm::format("%s", histogramTooltipBase)
        );
    }
}

void ImageViewer::insertImage(shared_ptr<Image> image, size_t index, bool shallSelect) {
    if (!image) {
        throw invalid_argument{"Image may not be null."};
    }

    for (auto button : mAnyImageButtons) {
        button->setEnabled(true);
    }

    auto button = new ImageButton{nullptr, image->name(), true};
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

    mImageButtonContainer->addChild((int)index, button);
    mImages.insert(begin(mImages) + index, image);

    // The following call will show the footer if there is not an image
    // with more than 1 layer.
    setUiVisible(isUiVisible());

    // Ensure the new image button will have the correct visibility state.
    setFilter(mFilter->value());

    requestLayoutUpdate();

    // First image got added, let's select it.
    if (index == 0 || shallSelect) {
        selectImage(image);
        resizeToFitImage(image);
    }
}

void ImageViewer::removeImage(shared_ptr<Image> image) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    auto nextCandidate = nextImage(image, Forward);
    // If we rolled over, let's rather use the previous image.
    // We don't want to jumpt to the beginning when deleting the
    // last image in our list.
    if (imageId(nextCandidate) < id) {
        nextCandidate = nextImage(image, Backward);
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    requestFocus();

    mImages.erase(begin(mImages) + id);
    mImageButtonContainer->removeChild(id);

    if (mImages.empty()) {
        selectImage(nullptr);
        selectReference(nullptr);

        for (auto button : mAnyImageButtons) {
            button->setEnabled(false);
        }

        return;
    }

    if (mCurrentImage == image) {
        selectImage(nextCandidate);
    }

    if (mCurrentReference == image) {
        selectReference(nextCandidate);
    }
}

void ImageViewer::reloadImage(shared_ptr<Image> image) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    int referenceId = imageId(mCurrentReference);

    auto newImage = tryLoadImage(image->filename(), image->channelSelector());
    if (newImage) {
        removeImage(image);
        insertImage(newImage, id, true);
    }

    if (referenceId != -1) {
        selectReference(mImages[referenceId]);
    }
}

void ImageViewer::reloadAllImages() {
    int id = imageId(mCurrentImage);
    for (size_t i = 0; i < mImages.size(); ++i) {
        reloadImage(mImages[i]);
    }

    if (id != -1) {
        selectImage(mImages[id]);
    }
}

void ImageViewer::selectImage(const shared_ptr<Image>& image) {
    for (auto button : mCurrentImageButtons) {
        button->setEnabled(image != nullptr);
    }

    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(false);
        }

        mCurrentImage = nullptr;
        mImageCanvas->setImage(nullptr);

        // Clear layer buttons
        while (mLayerButtonContainer->childCount() > 0) {
            mLayerButtonContainer->removeChild(mLayerButtonContainer->childCount() - 1);
        }

        requestLayoutUpdate();
        return;
    }

    size_t id = (size_t)max(0, imageId(image));

    // Don't do anything if the image that wants to be selected is not visible.
    if (!mImageButtonContainer->childAt((int)id)->visible()) {
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
        auto button = new ImageButton{mLayerButtonContainer, layer.empty() ? "<root>" : layer, false};
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

    // Ensure the currently active image button is always fully on-screen
    Widget* activeImageButton = nullptr;
    for (Widget* widget : mImageButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeImageButton = widget;
            break;
        }
    }

    if (activeImageButton) {
        float divisor = mScrollContent->height() - mImageScrollContainer->height();
        if (divisor > 0) {
            mImageScrollContainer->setScroll(clamp(
                mImageScrollContainer->scroll(),
                (activeImageButton->position().y() + activeImageButton->height() - mImageScrollContainer->height()) / divisor,
                activeImageButton->position().y() / divisor
            ));
        }
    }
}

void ImageViewer::selectLayer(string layer) {
    // If the layer does not exist, select the first layer.
    size_t id = (size_t)max(0, layerId(layer));

    auto& buttons = mLayerButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentLayer = layerName(id);
    mImageCanvas->setRequestedLayer(mCurrentLayer);

    // Ensure the currently active layer button is always fully on-screen
    Widget* activeLayerButton = nullptr;
    for (Widget* widget : mLayerButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeLayerButton = widget;
            break;
        }
    }

    // Ensure the currently active layer button is always fully on-screen
    if (activeLayerButton) {
        mLayerButtonContainer->setPosition(Vector2i{
            clamp(
                mLayerButtonContainer->position().x(),
                -activeLayerButton->position().x(),
                mSize.x() - activeLayerButton->position().x() - activeLayerButton->width()
            ),
            0
        });
    }
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

    size_t id = (size_t)max(0, imageId(image));

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

    // Ensure the currently active reference button is always fully on-screen
    Widget* activeReferenceButton = nullptr;
    for (Widget* widget : mImageButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isReference()) {
            activeReferenceButton = widget;
            break;
        }
    }

    if (activeReferenceButton) {
        float divisor = mScrollContent->height() - mImageScrollContainer->height();
        if (divisor > 0) {
            mImageScrollContainer->setScroll(clamp(
                mImageScrollContainer->scroll(),
                (activeReferenceButton->position().y() + activeReferenceButton->height() - mImageScrollContainer->height()) / divisor,
                activeReferenceButton->position().y() / divisor
            ));
        }
    }
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
            maximum = max(maximum, val);
            minimum = min(minimum, val);
        }
    }

    float factor = 1.0f / (maximum - minimum);
    setExposure(log2(factor));
    setOffset(-minimum * factor);
}

void ImageViewer::resetImage() {
    setExposure(0);
    setOffset(0);
    mImageCanvas->resetTransform();
}

void ImageViewer::setTonemap(ETonemap tonemap) {
    mImageCanvas->setTonemap(tonemap);
    auto& buttons = mTonemapButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed((ETonemap)i == tonemap);
    }
}

void ImageViewer::setMetric(EMetric metric) {
    mImageCanvas->setMetric(metric);
    auto& buttons = mMetricButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed((EMetric)i == metric);
    }
}

void ImageViewer::resizeToFitImage(const shared_ptr<Image>& image) {
    if (!image || isMaximized()) {
        return;
    }

    Vector2i requiredSize = image->size();

    // Convert from image pixel coordinates to nanogui coordinates.
    requiredSize = (requiredSize.cast<float>() / pixelRatio()).cast<int>();

    // Take into account the size of the UI.
    if (mSidebar->visible()) {
        requiredSize.x() += mSidebar->fixedWidth();
    }

    if (mFooter->visible()) {
        requiredSize.y() += mFooter->fixedHeight();
    }

    // Only increase our current size if we are larger than the current size of the window.
    setSize(mSize.cwiseMax(requiredSize));
}

void ImageViewer::resizeToFitAllImages() {
    for (const auto& image : mImages) {
        resizeToFitImage(image);
    }
}

bool ImageViewer::setFilter(const string& filter) {
    mFilter->setValue(filter);
    mRequiresFilterUpdate = true;
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
    if (!shouldBeVisible) {
        mIsDraggingSidebar = false;
    }

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

    requestLayoutUpdate();
}

void ImageViewer::toggleHelpWindow() {
    if (mHelpWindow) {
        mHelpWindow->dispose();
        mHelpWindow = nullptr;
    } else {
        mHelpWindow = new HelpWindow{this, [this] { toggleHelpWindow(); }};
        mHelpWindow->center();
        mHelpWindow->requestFocus();
    }

    requestLayoutUpdate();
}

void ImageViewer::openImageDialog() {
    vector<string> paths = file_dialog(
    {
        {"exr",  "OpenEXR Image"},
        {"hdr",  "HDR Image"},
        {"bmp",  "Bitmap Image File"},
        {"gif",  "Graphics Interchange Format Image"},
        {"jpg",  "JPEG Image"},
        {"jpeg", "JPEG Image"},
        {"pic",  "PIC Image"},
        {"png",  "Portable Network Graphics Image"},
        {"pnm",  "Portable Any Map Image"},
        {"psd",  "PSD Image"},
        {"tga",  "Truevision TGA Image"},
    }, false, true);

    for (size_t i = 0; i < paths.size(); ++i) {
        const string& imageFile = paths[i];
        bool shallSelect = i == paths.size() - 1;
        ThreadPool::singleWorker().enqueueTask([imageFile, shallSelect, this] {
            auto image = tryLoadImage(imageFile, "");
            if (image) {
                mImagesToAdd->push({shallSelect, image});
            }
        });
    }

    // Make sure we gain focus after seleting a file to be loaded.
    glfwFocusWindow(mGLFWWindow);
}

void ImageViewer::saveImageDialog() {
    if (!mCurrentImage) {
        return;
    }

    string path = file_dialog(
    {
        //{"exr",  "OpenEXR image"},
        {"hdr",  "HDR image"},
        {"bmp",  "Bitmap Image File"},
        {"jpg",  "JPEG image"},
        {"jpeg", "JPEG image"},
        {"png",  "Portable Network Graphics image"},
        {"tga",  "Truevision TGA image"},
    }, true);

    if (path.empty()) {
        return;
    }

    try {
        mImageCanvas->saveImage(path);
    } catch (invalid_argument e) {
        new MessageDialog(
            this,
            MessageDialog::Type::Warning,
            "Error",
            tfm::format("Failed to save image: %s", e.what())
        );
    }

    // Make sure we gain focus after seleting a file to be loaded.
    glfwFocusWindow(mGLFWWindow);
}

void ImageViewer::updateFilter() {
    string filter = mFilter->value();
    string imagePart = filter;
    string layerPart = "";

    auto colonPos = filter.find_last_of(':');
    if (colonPos != string::npos) {
        imagePart = filter.substr(0, colonPos);
        layerPart = filter.substr(colonPos + 1);
    }

    // Image filtering
    {
        // Checks whether an image matches the filter.
        // This is the case if the image name matches the image part
        // and at least one of the image's layers matches the layer part.
        auto doesImageMatch = [&](const shared_ptr<Image>& image) {
            bool doesMatch = matches(image->name(), imagePart);
            if (doesMatch) {
                bool anyLayersMatch = false;
                for (const auto& layer : image->layers()) {
                    if (matches(layer, layerPart)) {
                        anyLayersMatch = true;
                        break;
                    }
                }

                if (!anyLayersMatch) {
                    doesMatch = false;
                }
            }

            return doesMatch;
        };

        vector<string> activeImageNames;
        size_t id = 1;
        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            ib->setVisible(doesImageMatch(mImages[i]));
            if (ib->visible()) {
                ib->setId(id++);
                activeImageNames.emplace_back(mImages[i]->name());
            }
        }

        int beginOffset = 0, endOffset = 0;
        if (!activeImageNames.empty()) {
            string first = activeImageNames.front();
            int firstSize = (int)first.size();
            if (firstSize > 0) {
                bool allStartWithSameChar;
                do {
                    char firstChar = first[beginOffset];
                    allStartWithSameChar = all_of(
                        begin(activeImageNames),
                        end(activeImageNames),
                        [firstChar, beginOffset](const string& name) {
                            return beginOffset < (int)name.size() && name[beginOffset] == firstChar;
                        }
                    );

                    if (allStartWithSameChar) {
                        ++beginOffset;
                    }
                } while (allStartWithSameChar && beginOffset < firstSize);

                bool allEndWithSameChar;
                do {
                    char lastChar = first[firstSize - endOffset - 1];
                    allEndWithSameChar = all_of(
                        begin(activeImageNames),
                        end(activeImageNames),
                        [lastChar, endOffset](const string& name) {
                            int index = (int)name.size() - endOffset - 1;
                            return index >= 0 && name[index] == lastChar;
                        }
                    );

                    if (allEndWithSameChar) {
                        ++endOffset;
                    }
                } while (allEndWithSameChar && endOffset < firstSize);
            }
        }

        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            if (ib->visible()) {
                ib->setHighlightRange(beginOffset, endOffset);
            }
        }

        if (mCurrentImage && !doesImageMatch(mCurrentImage)) {
            selectImage(nthVisibleImage(0));
        }

        if (mCurrentReference && !matches(mCurrentReference->name(), imagePart)) {
            selectReference(nullptr);
        }
    }

    // Layer filtering
    if (mCurrentImage)
    {
        size_t id = 1;
        const auto& buttons = mLayerButtonContainer->children();
        for (Widget* button : buttons) {
            ImageButton* ib = dynamic_cast<ImageButton*>(button);
            ib->setVisible(matches(ib->caption(), layerPart));
            if (ib->visible()) {
                ib->setId(id++);
            }
        }

        if (!matches(mCurrentLayer, layerPart)) {
            selectLayer(nthVisibleLayer(0));
        }
    }

    requestLayoutUpdate();
}

void ImageViewer::updateLayout() {
    int sidebarWidth = visibleSidebarWidth();
    int footerHeight = visibleFooterHeight();
    mImageCanvas->setFixedSize(mSize - Vector2i{sidebarWidth - 1, footerHeight - 1});
    mSidebar->setFixedHeight(mSize.y() - footerHeight);

    mHelpButton->setPosition(Vector2i{mSidebar->fixedWidth() - 38, 5});
    mSidebarLayout->setFixedWidth(mSidebar->fixedWidth());

    mVerticalScreenSplit->setFixedSize(mSize);
    mImageScrollContainer->setFixedHeight(
        mSize.y() - mImageScrollContainer->position().y() - footerHeight
    );

    performLayout();

    // With a changed layout the relative position of the mouse
    // within children changes and therefore should get updated.
    // nanogui does not handle this for us.
    double x, y;
    glfwGetCursorPos(mGLFWWindow, &x, &y);
    cursorPosCallbackEvent(x, y);
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
            caption += string{" – "} + channelsString;
        } else {
            caption += string{" – "} + mCurrentLayer;
            if (channels.size() == 1) {
                caption += string{"."} + channelsString;
            } else {
                caption += string{".("} + channelsString + ")";
            }
        }

        vector<float> values = mImageCanvas->getValuesAtNanoPos(mousePos() - mImageCanvas->position());
        Vector2i imageCoords = mImageCanvas->getImageCoords(*mCurrentImage, mousePos() - mImageCanvas->position());
        TEV_ASSERT(values.size() >= channels.size(), "Should obtain a value for every existing channel.");

        string valuesString;
        for (size_t i = 0; i < channels.size(); ++i) {
            valuesString += tfm::format("%.2f,", values[i]);
        }
        valuesString.pop_back();

        caption += tfm::format(" – @(%d,%d)%s", imageCoords.x(), imageCoords.y(), valuesString);
    }

    setCaption(caption);
}

string ImageViewer::layerName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    return mCurrentImage->layers().at(index);
}

int ImageViewer::layerId(const string& layer) const {
    if (!mCurrentImage) {
        return 0;
    }

    const auto& layers = mCurrentImage->layers();
    auto pos = static_cast<size_t>(distance(begin(layers), find(begin(layers), end(layers), layer)));
    return pos >= layers.size() ? -1 : (int)pos;
}

int ImageViewer::imageId(const shared_ptr<Image>& image) const {
    auto pos = static_cast<size_t>(distance(begin(mImages), find(begin(mImages), end(mImages), image)));
    return pos >= mImages.size() ? -1 : (int)pos;
}

string ImageViewer::nextLayer(const string& layer, EDirection direction) {
    if (mLayerButtonContainer->childCount() == 0) {
        return mCurrentLayer;
    }

    int dir = direction == Forward ? 1 : -1;

    // If the layer does not exist, start at index 0.
    int startId = max(0, layerId(layer));

    int id = startId;
    do {
        id = (id + mLayerButtonContainer->childCount() + dir) % mLayerButtonContainer->childCount();
    } while (!mLayerButtonContainer->childAt(id)->visible() && id != startId);

    return layerName(id);
}

string ImageViewer::nthVisibleLayer(size_t n) {
    string lastVisible = mCurrentLayer;
    for (int i = 0; i < mLayerButtonContainer->childCount(); ++i) {
        if (mLayerButtonContainer->childAt(i)->visible()) {
            lastVisible = layerName(i);
            if (n == 0) {
                break;
            }
            --n;
        }
    }
    return lastVisible;
}

shared_ptr<Image> ImageViewer::nextImage(const shared_ptr<Image>& image, EDirection direction) {
    if (mImages.empty()) {
        return nullptr;
    }

    int dir = direction == Forward ? 1 : -1;

    // If the image does not exist, start at image 0.
    int startId = max(0, imageId(image));

    int id = startId;
    do {
        id = (id + mImageButtonContainer->childCount() + dir) % mImageButtonContainer->childCount();
    } while (!mImageButtonContainer->childAt(id)->visible() && id != startId);

    return mImages[id];
}

shared_ptr<Image> ImageViewer::nthVisibleImage(size_t n) {
    shared_ptr<Image> lastVisible = nullptr;
    for (size_t i = 0; i < mImages.size(); ++i) {
        if (mImageButtonContainer->children()[i]->visible()) {
            lastVisible = mImages[i];
            if (n == 0) {
                break;
            }
            --n;
        }
    }
    return lastVisible;
}

TEV_NAMESPACE_END
