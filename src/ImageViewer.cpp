// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ImageViewer.h>

#include <clip.h>

#include <filesystem/path.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/entypo.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/popupbutton.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace Eigen;
using namespace filesystem;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageViewer::ImageViewer(const shared_ptr<BackgroundImagesLoader>& imagesLoader, bool processPendingDrops)
: nanogui::Screen{Vector2i{1024, 799}, "tev"}, mImagesLoader{imagesLoader} {
    // At this point we no longer need the standalone console (if it exists).
    toggleConsole();

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

    // Tonemapping section
    {
        auto panel = new Widget{mSidebarLayout};
        panel->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});
        new Label{panel, "Tonemapping", "sans-bold", 25};
        panel->setTooltip(
            "Various tonemapping options. Hover the individual controls to learn more!"
        );

        // Exposure label and slider
        {
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
        }

        // Offset/Gamma label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->setLayout(new GridLayout{Orientation::Vertical, 2, Alignment::Fill, 5, 0});

            mOffsetLabel = new Label{panel, "", "sans-bold", 15};

            mOffsetSlider = new Slider{panel};
            mOffsetSlider->setRange({-1.0f, 1.0f});
            mOffsetSlider->setCallback([this](float value) {
                setOffset(value);
            });
            setOffset(0);

            mGammaLabel = new Label{panel, "", "sans-bold", 15};

            mGammaSlider = new Slider{panel};
            mGammaSlider->setRange({0.01f, 5.0f});
            mGammaSlider->setCallback([this](float value) {
                setGamma(value);
            });
            setGamma(2.2f);

            panel->setTooltip(
                "The offset is added to the image after exposure has been applied.\n"
                "Keyboard shortcuts: O and Shift+O\n\n"
                "Gamma is the exponent used when gamma-tonemapping.\n"
                "Keyboard shortcuts: G and Shift+G\n\n"
            );
        }
    }

    // Exposure/offset buttons
    {
        auto buttonContainer = new Widget{mSidebarLayout};
        buttonContainer->setLayout(new GridLayout{Orientation::Horizontal, 3, Alignment::Fill, 5, 2});

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

        auto popupBtn = new PopupButton{buttonContainer, "", ENTYPO_ICON_BRUSH};
        popupBtn->setFontSize(15);
        popupBtn->setChevronIcon(0);
        popupBtn->setTooltip("Background Color");

        // Background color popup
        {
            auto popup = popupBtn->popup();
            popup->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            new Label{popup, "Background Color"};
            auto colorwheel = new ColorWheel{popup, mImageCanvas->backgroundColor()};
            colorwheel->setColor(popupBtn->backgroundColor());

            new Label{popup, "Background Alpha"};
            auto bgAlphaSlider = new Slider{popup};
            bgAlphaSlider->setRange({0.0f, 1.0f});
            bgAlphaSlider->setCallback([this](float value) {
                auto col = mImageCanvas->backgroundColor();
                mImageCanvas->setBackgroundColor(Color{
                    col.r(),
                    col.g(),
                    col.b(),
                    value,
                });
            });

            bgAlphaSlider->setValue(0);

            colorwheel->setCallback([bgAlphaSlider, this](const Color& value) {
                //popupBtn->setBackgroundColor(value);
                mImageCanvas->setBackgroundColor(Color{
                    value.r(),
                    value.g(),
                    value.b(),
                    bgAlphaSlider->value(),
                });
            });
        }
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

        // Histogram of selected image
        {
            panel = new Widget{mSidebarLayout};
            panel->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mHistogram = new MultiGraph{panel, ""};
        }

        // Fuzzy filter of open images
        {
            panel = new Widget{mSidebarLayout};
            panel->setLayout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

            mFilter = new TextBox{panel, ""};
            mFilter->setEditable(true);
            mFilter->setAlignment(TextBox::Alignment::Left);
            mFilter->setCallback([this](const string& filter) {
                return setFilter(filter);
            });

            mFilter->setPlaceholder("Find");
            mFilter->setTooltip(tfm::format(
                "Filters visible images and channel groups according to a supplied string. "
                "The string must have the format 'image:group'. "
                "Only images whose name contains 'image' and groups whose name contains 'group' will be visible.\n\n"
                "Keyboard shortcut:\n%s+P",
                HelpWindow::COMMAND
            ));

            mRegexButton = new Button{panel, "", ENTYPO_ICON_SEARCH};
            mRegexButton->setTooltip("Treat filter as regular expression");
            mRegexButton->setPushed(false);
            mRegexButton->setFlags(Button::ToggleButton);
            mRegexButton->setFontSize(15);
            mRegexButton->setChangeCallback([this](bool value) {
                setUseRegex(value);
            });
        }

        // Playback controls
        {
            auto playback = new Widget{mSidebarLayout};
            playback->setLayout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

            auto makePlaybackButton = [&](const string& name, bool enabled, function<void()> callback, int icon = 0, string tooltip = "") {
                auto button = new Button{playback, name, icon};
                button->setCallback(callback);
                button->setTooltip(tooltip);
                button->setFontSize(15);
                button->setEnabled(enabled);
                return button;
            };

            mPlayButton = makePlaybackButton("", true, []{}, ENTYPO_ICON_PLAY, "Play (Space)");
            mPlayButton->setFlags(Button::ToggleButton);

            mAnyImageButtons.push_back(makePlaybackButton("", false, [this] {
                selectImage(nthVisibleImage(0));
            }, ENTYPO_ICON_TO_START, "Front (Home)"));

            mAnyImageButtons.push_back(makePlaybackButton("", false, [this] {
                selectImage(nthVisibleImage(mImages.size()));
            }, ENTYPO_ICON_TO_END, "Back (End)"));

            mFpsTextBox = new IntBox<int>{playback, 24};
            mFpsTextBox->setDefaultValue("24");
            mFpsTextBox->setUnits("fps");
            mFpsTextBox->setEditable(true);
            mFpsTextBox->setAlignment(TextBox::Alignment::Right);
            mFpsTextBox->setMinMaxValues(1, 1000);
            mFpsTextBox->setSpinnable(true);
            mFpsTextBox->setFixedTextWidth(42);

            mPlaybackThread = thread{[&]() {
                while (mShallRunPlaybackThread) {
                    auto fps = clamp(mFpsTextBox->value(), 1, 1000);
                    auto sleepDuration = chrono::duration<float>{1.0f / fps};
                    this_thread::sleep_for(sleepDuration);

                    if (mPlayButton->pushed() && mTaskQueue.empty()) {
                        mTaskQueue.push([&]() {
                            selectImage(nextImage(mCurrentImage, Forward), false);
                        });
                        glfwPostEmptyEvent();
                    }
                }
            }};
        }

        // Save, refresh, load, close
        {
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
                auto* glfwWindow = screen()->glfwWindow();
                // There is no explicit access to the currently pressed modifier keys here, so we
                // need to directly ask GLFW. In case this is needed more often, it may be worth
                // inheriting Button and overriding mouseButtonEvent (similar to ImageButton).
                if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                    removeAllImages();
                } else {
                    removeImage(mCurrentImage);
                }
            }, ENTYPO_ICON_CIRCLED_CROSS, tfm::format("Close (%s+W); Close All (%s+Shift+W)", HelpWindow::COMMAND, HelpWindow::COMMAND)));

            spacer = new Widget{mSidebarLayout};
            spacer->setHeight(3);
        }

        // List of open images
        {
            mImageScrollContainer = new VScrollPanel{mSidebarLayout};
            mImageScrollContainer->setFixedWidth(mSidebarLayout->fixedWidth());

            mScrollContent = new Widget{mImageScrollContainer};
            mScrollContent->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

            mImageButtonContainer = new Widget{mScrollContent};
            mImageButtonContainer->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
        }
    }

    // Group selection
    {
        mFooter = new Widget{mVerticalScreenSplit};

        mGroupButtonContainer = new Widget{mFooter};
        mGroupButtonContainer->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});
        mGroupButtonContainer->setFixedHeight(25);
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

ImageViewer::~ImageViewer() {
    mShallRunPlaybackThread = false;
    if (mPlaybackThread.joinable()) {
        mPlaybackThread.join();
    }
}

bool ImageViewer::mouseButtonEvent(const Vector2i &p, int button, bool down, int modifiers) {
    // Check if the user performed mousedown on an imagebutton so we can mark it as being dragged.
    // This has to occur before Screen::mouseButtonEvent as the button would absorb the event.
    if (down) {
        if (mImageScrollContainer->contains(p)) {
            auto& buttons = mImageButtonContainer->children();

            Eigen::Vector2i relMousePos = (absolutePosition() + p) - mImageButtonContainer->absolutePosition();

            for (size_t i = 0; i < buttons.size(); ++i) {
                const auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->contains(relMousePos)) {
                    mDraggedImageButtonId = i;
                    mIsDraggingImageButton = true;
                    mDraggingStartPosition = (relMousePos - imgButton->position()).cast<float>();
                    break;
                }
            }
        }
    }

    if (Screen::mouseButtonEvent(p, button, down, modifiers)) {
        return true;
    }

    if (down && !mIsDraggingImageButton) {
        if (canDragSidebarFrom(p)) {
            mIsDraggingSidebar = true;
            mDraggingStartPosition = p.cast<float>();
            return true;
        } else if (mImageCanvas->contains(p)) {
            mIsDraggingImage = true;
            mDraggingStartPosition = p.cast<float>();
            return true;
        }
    } else {
        if (mIsDraggingImageButton) {
            requestLayoutUpdate();
        }

        mIsDraggingSidebar = false;
        mIsDraggingImage = false;
        mIsDraggingImageButton = false;
    }

    return false;
}

bool ImageViewer::mouseMotionEvent(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
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
        Vector2f relativeMovement = rel.cast<float>();
        auto* glfwWindow = screen()->glfwWindow();
        // There is no explicit access to the currently pressed modifier keys here, so we
        // need to directly ask GLFW.
        if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
            relativeMovement /= 10;
        } else if (glfwGetKey(glfwWindow, SYSTEM_COMMAND_LEFT) || glfwGetKey(glfwWindow, SYSTEM_COMMAND_RIGHT)) {
            relativeMovement /= std::log2(1.1f);
        }

        // If left mouse button is held, move the image with mouse movement
        if ((button & 1) != 0) {
            mImageCanvas->translate(relativeMovement);
        }

        // If middle mouse button is held, zoom in-out with up-down mouse movement
        if ((button & 4) != 0) {
            mImageCanvas->scale(relativeMovement.y() / 10.0f, mDraggingStartPosition);
        }
    } else if (mIsDraggingImageButton) {
        auto& buttons = mImageButtonContainer->children();
        Eigen::Vector2i relMousePos = (absolutePosition() + p) - mImageButtonContainer->absolutePosition();
        for (size_t i = 0; i < buttons.size(); ++i) {
            if (i == mDraggedImageButtonId) {
                continue;
            }
            auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
            if (imgButton->contains(relMousePos)) {
                Vector2i pos = imgButton->position();
                pos.y() += ((int)mDraggedImageButtonId - (int)i) * imgButton->size().y();
                imgButton->setPosition(pos);
                imgButton->mouseEnterEvent(relMousePos, false);

                moveImageInList(mDraggedImageButtonId, i);
                mDraggedImageButtonId = i;
                break;
            }
        }
        
        dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->setPosition(relMousePos - mDraggingStartPosition.cast<int>());
    }

    return false;
}

bool ImageViewer::dropEvent(const vector<string>& filenames) {
    if (Screen::dropEvent(filenames)) {
        return true;
    }

    for (size_t i = 0; i < filenames.size(); ++i) {
        mImagesLoader->enqueue(ensureUtf8(filenames[i]), "", i == filenames.size() - 1);
    }

    // Make sure we gain focus after dragging files into here.
    glfwFocusWindow(mGLFWWindow);
    return true;
}

bool ImageViewer::keyboardEvent(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboardEvent(key, scancode, action, modifiers)) {
        return true;
    }

    int numGroups = mGroupButtonContainer->childCount();

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
                if (idx >= 0 && idx < numGroups) {
                    selectGroup(nthVisibleGroup(idx));
                }
            } else {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    selectImage(image);
                }
            }
            return true;
        } else if (key == GLFW_KEY_HOME || key == GLFW_KEY_END) {
            const auto& image = nthVisibleImage(key == GLFW_KEY_HOME ? 0 : mImages.size());
            if (modifiers & GLFW_MOD_SHIFT) {
                if (mCurrentReference == image) {
                    selectReference(nullptr);
                } else {
                    selectReference(image);
                }
            } else {
                selectImage(image);
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
        } else if (key == GLFW_KEY_SPACE) {
            mPlayButton->setPushed(!mPlayButton->pushed());
            return true;
        } else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) {
            setVisible(false);
            return true;
        } else if (mCurrentImage && key == GLFW_KEY_C && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (mImageScrollContainer->focused()) {
                if (clip::set_text(mCurrentImage->name())) {
                    tlog::success() << "Image path copied to clipboard.";
                } else {
                    tlog::error() << "Failed to copy image path to clipboard.";
                }
            } else {
                auto imageSize = mCurrentImage->size();

                clip::image_spec imageMetadata;
                imageMetadata.width = imageSize.x();
                imageMetadata.height = imageSize.y();
                imageMetadata.bits_per_pixel = 32;
                imageMetadata.bytes_per_row = imageMetadata.bits_per_pixel / 8 * imageMetadata.width;

                imageMetadata.red_mask    = 0x000000ff;
                imageMetadata.green_mask  = 0x0000ff00;
                imageMetadata.blue_mask   = 0x00ff0000;
                imageMetadata.alpha_mask  = 0xff000000;
                imageMetadata.red_shift   = 0;
                imageMetadata.green_shift = 8;
                imageMetadata.blue_shift  = 16;
                imageMetadata.alpha_shift = 24;

                auto imageData = mImageCanvas->getLdrImageData(true);
                clip::image image(imageData.data(), imageMetadata);

                if (clip::set_image(image)) {
                    tlog::success() << "Image copied to clipboard.";
                } else {
                    tlog::error() << "Failed to copy image to clipboard.";
                }
            }
        } else if (key == GLFW_KEY_V && (modifiers & SYSTEM_COMMAND_MOD)) {
            // if (clip::has(clip::text_format())) {
            //     string path;
            //     if (!clip::get_text(path)) {
            //         tlog::error() << "Failed to paste text from clipboard.";
            //     } else {
            //         auto image = tryLoadImage(path, "");
            //         if (image) {
            //             addImage(image, true);
            //         } else {
            //             tlog::error() << tfm::format("Failed to load image from clipboard path: %s", path);
            //         }
            //     }
            // } else
            if (clip::has(clip::image_format())) {
                clip::image clipImage;
                if (!clip::get_image(clipImage)) {
                    tlog::error() << "Failed to paste image from clipboard.";
                } else {
                    tlog::info() << "Loading image from clipboard...";
                    stringstream imageStream;
                    imageStream
                        << "clip"
                        << string(reinterpret_cast<const char*>(&clipImage.spec()), sizeof(clip::image_spec))
                        << string(clipImage.data(), clipImage.spec().bytes_per_row * clipImage.spec().height)
                        ;

                    auto image = tryLoadImage(tfm::format("clipboard (%d)", ++mClipboardIndex), imageStream, "");
                    if (image) {
                        addImage(image, true);
                    } else {
                        tlog::error() << "Failed to load image from clipboard data.";
                    }
                }
            }
        }
    }

    // Keybindings which should respond to repeats
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_KP_ADD || key == GLFW_KEY_EQUAL ||
            key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
            float scaleAmount = 1.0f;
            if (modifiers & GLFW_MOD_SHIFT) {
                scaleAmount /= 10;
            } else if (modifiers & SYSTEM_COMMAND_MOD) {
                scaleAmount /= std::log2(1.1f);
            }

            if (key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
                scaleAmount = -scaleAmount;
            }

            mImageCanvas->scale(
                scaleAmount,
                mImageCanvas->position().cast<float>() + mImageCanvas->size().cast<float>() / 2
            );
        }

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

        if (mGammaSlider->enabled()) {
            if (key == GLFW_KEY_G) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    setGamma(gamma() - 0.1f);
                } else {
                    setGamma(gamma() + 0.1f);
                }
            }
        }

        if (key == GLFW_KEY_W && modifiers & SYSTEM_COMMAND_MOD) {
            if (modifiers & GLFW_MOD_SHIFT) {
                removeAllImages();
            } else {
                removeImage(mCurrentImage);
            }
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
                setTonemap(static_cast<ETonemap>((tonemap() + 1) % NumTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() + 1) % NumMetrics));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Forward));
            }
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() - 1 + NumTonemaps) % NumTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() - 1 + NumMetrics) % NumMetrics));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Backward));
            }
        }
    }

    return false;
}

void ImageViewer::drawContents() {
    // In case any images got loaded in the background, they sit around in mImagesLoader. Here is the
    // place where we actually add them to the GUI. Focus the application in case one of the
    // new images is meant to override the current selection.
    bool newFocus = false;
    try {
        while (true) {
            auto addition = mImagesLoader->tryPop();
            newFocus |= addition.shallSelect;
            addImage(addition.image, addition.shallSelect);
        }
    } catch (runtime_error) {
    }

    if (newFocus) {
        glfwFocusWindow(mGLFWWindow);
    }

    // mTaskQueue contains jobs that should be executed on the main thread. It is useful for handling
    // callbacks from background threads
    try {
        while (true) {
            mTaskQueue.tryPop()();
        }
    } catch (runtime_error) {
    }

    if (mRequiresFilterUpdate) {
        updateFilter();
        mRequiresFilterUpdate = false;
    }

    if (mRequiresLayoutUpdate) {
        Vector2i oldDraggedImageButtonPos{0, 0};
        auto& buttons = mImageButtonContainer->children();
        if (mIsDraggingImageButton) {
            oldDraggedImageButtonPos = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->position();
        }
        
        updateLayout();
        mRequiresLayoutUpdate = false;

        if (mIsDraggingImageButton) {
            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->setPosition(oldDraggedImageButtonPos);
        }
    }

    updateTitle();

    // Update histogram
    static const string histogramTooltipBase = "Histogram of color values. Adapts to the currently chosen channel group and error metric.";
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

    if (mIsDraggingImageButton && index <= mDraggedImageButtonId) {
        ++mDraggedImageButtonId;
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
    // with more than 1 group.
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

void ImageViewer::moveImageInList(size_t oldIndex, size_t newIndex) {
    if (oldIndex == newIndex) {
        return;
    }

    TEV_ASSERT(oldIndex < mImages.size(), "oldIndex must be smaller than the number of images.");
    TEV_ASSERT(newIndex < mImages.size(), "newIndex must be smaller than the number of images.");

    auto* button = mImageButtonContainer->childAt((int)oldIndex);
    button->incRef();
    mImageButtonContainer->removeChild((int)oldIndex);
    mImageButtonContainer->addChild((int)newIndex, button);
    button->decRef();

    auto startI = std::min(oldIndex, newIndex);
    auto endI = std::max(oldIndex, newIndex);
    for (size_t i = startI; i <= endI; ++i) {
        auto* curButton = dynamic_cast<ImageButton*>(mImageButtonContainer->childAt((int)i));
        curButton->setId(i+1);
    }

    auto img = mImages[oldIndex];
    mImages.erase(mImages.begin() + oldIndex);
    mImages.insert(mImages.begin() + newIndex, img);
    
    requestLayoutUpdate();
}

void ImageViewer::removeImage(shared_ptr<Image> image) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    if (mIsDraggingImageButton) {
        // If we're currently dragging the to-be-removed image, stop.
        if ((size_t)id == mDraggedImageButtonId) {
            requestLayoutUpdate();
            mIsDraggingImageButton = false;
        } else if ((size_t)id < mDraggedImageButtonId) {
            --mDraggedImageButtonId;
        }
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

void ImageViewer::removeAllImages() {
    if (mImages.empty())
        return;

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    requestFocus();

    for (size_t i = mImages.size(); i > 0; --i) {
        mImageButtonContainer->removeChild((int)(i - 1));
    }
    mImages.clear();

    // No images left to select
    selectImage(nullptr);
    selectReference(nullptr);
    for (auto button : mAnyImageButtons) {
        button->setEnabled(false);
    }
}

void ImageViewer::reloadImage(shared_ptr<Image> image) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    int referenceId = imageId(mCurrentReference);

    auto newImage = tryLoadImage(image->path(), image->channelSelector());
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

void ImageViewer::selectImage(const shared_ptr<Image>& image, bool stopPlayback) {
    if (stopPlayback) {
        mPlayButton->setPushed(false);
    }

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

        // Clear group buttons
        while (mGroupButtonContainer->childCount() > 0) {
            mGroupButtonContainer->removeChild(mGroupButtonContainer->childCount() - 1);
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

    // Clear group buttons
    while (mGroupButtonContainer->childCount() > 0) {
        mGroupButtonContainer->removeChild(mGroupButtonContainer->childCount() - 1);
    }

    size_t numGroups = mCurrentImage->channelGroups().size();
    for (size_t i = 0; i < numGroups; ++i) {
        auto group = groupName(i);
        auto button = new ImageButton{mGroupButtonContainer, group, false};
        button->setFontSize(15);
        button->setId(i + 1);

        button->setSelectedCallback([this, group]() {
            selectGroup(group);
        });
    }

    // Setting the filter again makes sure, that groups are correctly filtered.
    setFilter(mFilter->value());
    updateLayout();

    // This will automatically fall back to the root group if the current
    // group isn't found.
    selectGroup(mCurrentGroup);

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

void ImageViewer::selectGroup(string group) {
    // If the group does not exist, select the first group.
    size_t id = (size_t)max(0, groupId(group));

    auto& buttons = mGroupButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentGroup = groupName(id);
    mImageCanvas->setRequestedChannelGroup(mCurrentGroup);

    // Ensure the currently active group button is always fully on-screen
    Widget* activeGroupButton = nullptr;
    for (Widget* widget : mGroupButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeGroupButton = widget;
            break;
        }
    }

    // Ensure the currently active group button is always fully on-screen
    if (activeGroupButton) {
        mGroupButtonContainer->setPosition(Vector2i{
            clamp(
                mGroupButtonContainer->position().x(),
                -activeGroupButton->position().x(),
                mSize.x() - activeGroupButton->position().x() - activeGroupButton->width()
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

void ImageViewer::setGamma(float value) {
    value = round(value, 2.0f);
    mGammaSlider->setValue(value);
    mGammaLabel->setCaption(tfm::format("Gamma: %+.2f", value));

    mImageCanvas->setGamma(value);
}

void ImageViewer::normalizeExposureAndOffset() {
    if (!mCurrentImage) {
        return;
    }

    auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);

    float minimum = numeric_limits<float>::max();
    float maximum = numeric_limits<float>::min();
    for (const auto& channelName : channels) {
        const auto& channel = mCurrentImage->channel(channelName);
        for (DenseIndex i = 0; i < channel->count(); ++i) {
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
    setGamma(2.2f);
    mImageCanvas->resetTransform();
}

void ImageViewer::setTonemap(ETonemap tonemap) {
    mImageCanvas->setTonemap(tonemap);
    auto& buttons = mTonemapButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->setPushed((ETonemap)i == tonemap);
    }

    mGammaSlider->setEnabled(tonemap == ETonemap::Gamma);
    mGammaLabel->setColor(
        tonemap == ETonemap::Gamma ? mGammaLabel->theme()->mTextColor : Color{0.5f, 1.0f}
    );
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

bool ImageViewer::useRegex() {
    return mRegexButton->pushed();
}

void ImageViewer::setUseRegex(bool value) {
    mRegexButton->setPushed(value);
    mRequiresFilterUpdate = true;
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
        // has more than the root group.
        if (image->channelGroups().size() > 1) {
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
        // HDR formats
        {"exr",  "OpenEXR image"},
        {"hdr",  "HDR image"},
        {"pfm",  "Portable Float Map image"},
        // LDR formats
        {"bmp",  "Bitmap Image File"},
        {"gif",  "Graphics Interchange Format image"},
        {"jpg",  "JPEG image"},
        {"jpeg", "JPEG image"},
        {"pic",  "PIC image"},
        {"pgm",  "Portable GrayMap image"},
        {"png",  "Portable Network Graphics image"},
        {"pnm",  "Portable AnyMap image"},
        {"ppm",  "Portable PixMap image"},
        {"psd",  "PSD image"},
        {"tga",  "Truevision TGA image"},
    }, false, true);

    for (size_t i = 0; i < paths.size(); ++i) {
        path imageFile = ensureUtf8(paths[i]);
        bool shallSelect = i == paths.size() - 1;
        mImagesLoader->enqueue(imageFile, "", shallSelect);
    }

    // Make sure we gain focus after seleting a file to be loaded.
    glfwFocusWindow(mGLFWWindow);
}

void ImageViewer::saveImageDialog() {
    if (!mCurrentImage) {
        return;
    }

    path path = ensureUtf8(file_dialog(
    {
        {"exr",  "OpenEXR image"},
        {"hdr",  "HDR image"},
        {"bmp",  "Bitmap Image File"},
        {"jpg",  "JPEG image"},
        {"jpeg", "JPEG image"},
        {"png",  "Portable Network Graphics image"},
        {"tga",  "Truevision TGA image"},
    }, true));

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
    string groupPart = "";

    auto colonPos = filter.find_last_of(':');
    if (colonPos != string::npos) {
        imagePart = filter.substr(0, colonPos);
        groupPart = filter.substr(colonPos + 1);
    }

    // Image filtering
    {
        // Checks whether an image matches the filter.
        // This is the case if the image name matches the image part
        // and at least one of the image's groups matches the group part.
        auto doesImageMatch = [&](const shared_ptr<Image>& image) {
            bool doesMatch = matchesFuzzyOrRegex(image->name(), imagePart, useRegex());
            if (doesMatch) {
                bool anyGroupsMatch = false;
                for (const auto& group : image->channelGroups()) {
                    if (matchesFuzzyOrRegex(group.name, groupPart, useRegex())) {
                        anyGroupsMatch = true;
                        break;
                    }
                }

                if (!anyGroupsMatch) {
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
                    int len = codePointLength(first[beginOffset]);

                    allStartWithSameChar = all_of(
                        begin(activeImageNames),
                        end(activeImageNames),
                        [&first, beginOffset, len](const string& name) {
                            if (beginOffset + len > (int)name.size()) {
                                return false;
                            }
                            for (int i = beginOffset; i < beginOffset + len; ++i) {
                                if (name[i] != first[i]) {
                                    return false;
                                }
                            }
                            return true;
                        }
                    );

                    if (allStartWithSameChar) {
                        beginOffset += len;
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

        if (mCurrentReference && !matchesFuzzyOrRegex(mCurrentReference->name(), imagePart, useRegex())) {
            selectReference(nullptr);
        }
    }

    // Group filtering
    if (mCurrentImage) {
        size_t id = 1;
        const auto& buttons = mGroupButtonContainer->children();
        for (Widget* button : buttons) {
            ImageButton* ib = dynamic_cast<ImageButton*>(button);
            ib->setVisible(matchesFuzzyOrRegex(ib->caption(), groupPart, useRegex()));
            if (ib->visible()) {
                ib->setId(id++);
            }
        }

        if (!matchesFuzzyOrRegex(mCurrentGroup, groupPart, useRegex())) {
            selectGroup(nthVisibleGroup(0));
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
    mFilter->setFixedWidth(mSidebar->fixedWidth() - 50);
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
        auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);
        // Remove duplicates
        channels.erase(unique(begin(channels), end(channels)), end(channels));

        auto channelTails = channels;
        transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);

        caption = mCurrentImage->shortName();
        caption += string{" – "} + mCurrentGroup;

        vector<float> values = mImageCanvas->getValuesAtNanoPos(mousePos() - mImageCanvas->position(), channels);
        Vector2i imageCoords = mImageCanvas->getImageCoords(*mCurrentImage, mousePos() - mImageCanvas->position());
        TEV_ASSERT(values.size() >= channelTails.size(), "Should obtain a value for every existing channel.");

        string valuesString;
        for (size_t i = 0; i < channelTails.size(); ++i) {
            valuesString += tfm::format("%.2f,", values[i]);
        }
        valuesString.pop_back();
        valuesString += " / 0x";
        for (size_t i = 0; i < channelTails.size(); ++i) {
            float tonemappedValue = channelTails[i] == "A" ? values[i] : toSRGB(values[i]);
            unsigned char discretizedValue = (char)(tonemappedValue * 255 + 0.5f);
            valuesString += tfm::format("%02X", discretizedValue);
        }

        caption += tfm::format(" – @(%d,%d)%s", imageCoords.x(), imageCoords.y(), valuesString);
        caption += tfm::format(" – %d%%", (int)std::round(mImageCanvas->extractScale() * 100));
    }

    setCaption(caption);
}

string ImageViewer::groupName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    return mCurrentImage->channelGroups().at(index).name;
}

int ImageViewer::groupId(const string& groupName) const {
    if (!mCurrentImage) {
        return 0;
    }

    const auto& groups = mCurrentImage->channelGroups();
    size_t pos = 0;
    for (; pos < groups.size(); ++pos) {
        if (groups[pos].name == groupName) {
            break;
        }
    }

    return pos >= groups.size() ? -1 : (int)pos;
}

int ImageViewer::imageId(const shared_ptr<Image>& image) const {
    auto pos = static_cast<size_t>(distance(begin(mImages), find(begin(mImages), end(mImages), image)));
    return pos >= mImages.size() ? -1 : (int)pos;
}

string ImageViewer::nextGroup(const string& group, EDirection direction) {
    if (mGroupButtonContainer->childCount() == 0) {
        return mCurrentGroup;
    }

    int dir = direction == Forward ? 1 : -1;

    // If the group does not exist, start at index 0.
    int startId = max(0, groupId(group));

    int id = startId;
    do {
        id = (id + mGroupButtonContainer->childCount() + dir) % mGroupButtonContainer->childCount();
    } while (!mGroupButtonContainer->childAt(id)->visible() && id != startId);

    return groupName(id);
}

string ImageViewer::nthVisibleGroup(size_t n) {
    string lastVisible = mCurrentGroup;
    for (int i = 0; i < mGroupButtonContainer->childCount(); ++i) {
        if (mGroupButtonContainer->childAt(i)->visible()) {
            lastVisible = groupName(i);
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
