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

#include <tev/Image.h>
#include <tev/ImageViewer.h>
#include <tev/WaylandClipboard.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/ImageSaver.h>
#include <tev/imageio/StbiLdrImageSaver.h>

#include <clip.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/combobox.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/popupbutton.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>
#ifdef __APPLE__
#    include <nanogui/metal.h>
#endif

#include <chrono>
#include <limits>
#include <stdexcept>

using namespace nanogui;
using namespace std;

namespace tev {

static const int SIDEBAR_MIN_WIDTH = 230;
static const float CROP_MIN_SIZE = 3;

static const vector<pair<EWpPrimaries, string_view>> PRIMARIES = {
    {EWpPrimaries::SRGB,        "sRGB"        },
    {EWpPrimaries::BT2020,      "BT.2020"     },
    {EWpPrimaries::DCIP3,       "DCI P3"      },
    {EWpPrimaries::DisplayP3,   "Display P3"  },
    {EWpPrimaries::AdobeRGB,    "Adobe RGB"   },
    {EWpPrimaries::ProPhotoRGB, "ProPhoto RGB"},
    {EWpPrimaries::NTSC,        "NTSC"        },
    {EWpPrimaries::PAL,         "PAL"         },
    {EWpPrimaries::PALM,        "PAL-M"       },
    {EWpPrimaries::Film,        "Generic Film"},
    {EWpPrimaries::CIE1931XYZ,  "CIE 1931 XYZ"},
};

static const vector<pair<ituth273::ETransfer, string_view>> TRANSFERS = {
    {ituth273::ETransfer::Linear,         "Linear"         },
    {ituth273::ETransfer::SRGB,           "sRGB"           },
    {ituth273::ETransfer::PQ,             "PQ"             },
    {ituth273::ETransfer::HLG,            "HLG"            },
    {ituth273::ETransfer::Gamma22,        "Gamma 2.2"      },
    {ituth273::ETransfer::Gamma28,        "Gamma 2.8"      },
    {ituth273::ETransfer::Log100,         "Log100"         },
    {ituth273::ETransfer::Log100Sqrt10,   "Log100 Sqrt10"  },
    {ituth273::ETransfer::BT709,          "BT.709/601/2020"},
    // Same as above
    // {ituth273::ETransfer::BT601,          "BT.601"          },
    // {ituth273::ETransfer::BT202010bit,    "BT.2020 10-bit"  },
    // {ituth273::ETransfer::BT202012bit,    "BT.2020"         },
    {ituth273::ETransfer::BT1361Extended, "BT.1361 Ext."   },
    {ituth273::ETransfer::SMPTE240,       "SMPTE 240M"     },
    {ituth273::ETransfer::SMPTE428,       "SMPTE ST 428-1" },
    {ituth273::ETransfer::IEC61966_2_4,   "IEC 61966-2-4"  },
};

ImageViewer::ImageViewer(
    const Vector2i& size, const shared_ptr<BackgroundImagesLoader>& imagesLoader, weak_ptr<Ipc> ipc, bool maximize, bool showUi, bool floatBuffer
) :
    Screen{size, "tev", true, maximize, false, true, true, floatBuffer}, mImagesLoader{imagesLoader}, mIpc{ipc}, mMaximizedLaunch{maximize} {

    // At this point we no longer need the standalone console (if it exists).
    toggleConsole();

    // Get monitor configuration to figure out how large the tev window may maximally become. This will later get overwritten once
    // glfwGetWindowCurrentMonitor() works (it does not while the window is still getting constructed).
    {
        int monitorCount;
        auto** monitors = glfwGetMonitors(&monitorCount);
        if (monitors && monitorCount > 0) {
            Vector2i monitorMin{numeric_limits<int>::max(), numeric_limits<int>::max()},
                monitorMax{numeric_limits<int>::min(), numeric_limits<int>::min()};

            for (int i = 0; i < monitorCount; ++i) {
                Vector2i pos, size;
                glfwGetMonitorWorkarea(monitors[i], &pos.x(), &pos.y(), &size.x(), &size.y());
                monitorMin = min(monitorMin, pos);
                monitorMax = max(monitorMax, pos + size);
            }

            mMinWindowPos = monitorMin;
            mMaxWindowSize = min(mMaxWindowSize, Vector2f{max(monitorMax - monitorMin, Vector2i{1024, 800})});
        }
    }

    // Try to get the current monitor size right away. Better to have it early. The function will get called again before every draw to
    // handle monitor changes as well as the case where glfwGetWindowCurrentMonitor() did not work yet.
    updateCurrentMonitorSize();

    m_background = Color{0.23f, 1.0f};

    mVerticalScreenSplit = new Widget{this};
    mVerticalScreenSplit->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

    auto horizontalScreenSplit = new Widget(mVerticalScreenSplit);
    horizontalScreenSplit->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

    mSidebar = new VScrollPanel{horizontalScreenSplit};
    mSidebar->set_fixed_width(SIDEBAR_MIN_WIDTH);
    mSidebar->set_visible(showUi);

    auto tmp = new Widget{mSidebar};
    mHelpButton = new Button{tmp, "", FA_QUESTION};
    mHelpButton->set_change_callback([this](bool) { toggleHelpWindow(); });
    mHelpButton->set_font_size(15);
    mHelpButton->set_tooltip("Information about using tev.");
    mHelpButton->set_flags(Button::ToggleButton);

    mSidebarLayout = new Widget{tmp};
    mSidebarLayout->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    mImageCanvas = new ImageCanvas{horizontalScreenSplit};
    mImageCanvas->setPixelRatio(pixel_ratio());

    // Tonemapping sectionim
    {
        auto panel = new Widget{mSidebarLayout};
        panel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});
        new Label{panel, "Tonemapping", "sans-bold", 25};
        panel->set_tooltip("Various tonemapping options. Hover the individual controls to learn more!");

        // Exposure label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mExposureLabel = new Label{panel, "", "sans-bold", 15};

            mExposureSlider = new Slider{panel};
            mExposureSlider->set_range({-5.0f, 5.0f});
            mExposureSlider->set_callback([this](float value) { setExposure(value); });
            setExposure(0);

            panel->set_tooltip(
                "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure.\n\n"
                "Keyboard shortcuts:\nE and Shift+E"
            );
        }

        // Offset/Gamma label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->set_layout(new GridLayout{Orientation::Vertical, 2, Alignment::Fill, 5, 0});

            mOffsetLabel = new Label{panel, "", "sans-bold", 15};

            mOffsetSlider = new Slider{panel};
            mOffsetSlider->set_range({-1.0f, 1.0f});
            mOffsetSlider->set_callback([this](float value) { setOffset(value); });
            setOffset(0);

            mGammaLabel = new Label{panel, "", "sans-bold", 15};

            mGammaSlider = new Slider{panel};
            mGammaSlider->set_range({0.01f, 5.0f});
            mGammaSlider->set_callback([this](float value) { setGamma(value); });
            setGamma(2.2f);

            panel->set_tooltip(
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
        buttonContainer->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

        auto makeButton = [&](string_view name, function<void()> callback, int icon = 0, string_view tooltip = "") {
            auto button = new Button{buttonContainer, name, icon};
            button->set_font_size(15);
            button->set_callback(callback);
            button->set_tooltip(tooltip);
            return button;
        };

        mCurrentImageButtons.push_back(makeButton(
            "Norm.",
            [this]() { normalizeExposureAndOffset(); },
            0,
            "Normalize image such that the smallest pixel value is displayed as 0 and the largest as 1.\n\n"
            "Shortcut: N"
        ));
        makeButton("Reset", [this]() { resetImage(); }, 0, "Shortcut: R");

        mHdrPopupButton = new PopupButton{buttonContainer, "HDR", 0};
        mHdrPopupButton->set_font_size(15);
        mHdrPopupButton->set_chevron_icon(0);

        auto addSpacer = [](Widget* current, int space) {
            auto row = new Widget{current};
            row->set_height(space);
        };

        {
            auto popup = mHdrPopupButton->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            new Label{popup, "HDR settings", "sans-bold", 20};
            addSpacer(popup, 10);

            mClipToLdrButton = new Button{popup, "Clip to LDR", 0};
            mClipToLdrButton->set_font_size(16);
            mClipToLdrButton->set_change_callback([this](bool value) { mImageCanvas->setClipToLdr(value); });
            mClipToLdrButton->set_tooltip(
                "Clips the image to [0,1] as if displayed on a low dynamic range (LDR) screen.\n\n"
                "Shortcut: U"
            );
            mClipToLdrButton->set_flags(Button::ToggleButton);

            addSpacer(popup, 10);

            new Label{popup, "Display white level"};

            addSpacer(popup, 5);

            auto whiteLevelContainer = new Widget{popup};
            whiteLevelContainer->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 0, 2});

            mDisplayWhiteLevelBox = new FloatBox<float>{whiteLevelContainer};
            mDisplayWhiteLevelBox->set_alignment(TextBox::Alignment::Right);
            mDisplayWhiteLevelBox->set_min_max_values(0.0f, 10000.0f);
            mDisplayWhiteLevelBox->set_fixed_width(90);
            mDisplayWhiteLevelBox->set_value(glfwGetWindowSdrWhiteLevel(m_glfw_window));
            mDisplayWhiteLevelBox->set_default_value(to_string(DEFAULT_IMAGE_WHITE_LEVEL));
            mDisplayWhiteLevelBox->set_units("nits");

            mDisplayWhiteLevelSettingComboBox = new ComboBox{
                whiteLevelContainer, {"System", "Custom", "Image"}
            };
            mDisplayWhiteLevelSettingComboBox->set_font_size(15);
            mDisplayWhiteLevelSettingComboBox->set_fixed_width(80);
            mDisplayWhiteLevelSettingComboBox->set_callback([this](int value) {
                setDisplayWhiteLevelSetting(static_cast<EDisplayWhiteLevelSetting>(value));
            });

            mDisplayWhiteLevelBox->set_callback([this](float value) {
                setDisplayWhiteLevelSetting(EDisplayWhiteLevelSetting::Custom);
                setDisplayWhiteLevel(value);
            });

            addSpacer(popup, 10);

            new Label{popup, "Best guess image white level"};

            addSpacer(popup, 5);

            mImageWhiteLevelBox = new FloatBox<float>{popup};
            mImageWhiteLevelBox->set_alignment(TextBox::Alignment::Right);
            mImageWhiteLevelBox->set_min_max_values(0.0, 10000.0f);
            mImageWhiteLevelBox->set_fixed_width(90);
            mImageWhiteLevelBox->set_value(DEFAULT_IMAGE_WHITE_LEVEL);
            mImageWhiteLevelBox->set_default_value(to_string(DEFAULT_IMAGE_WHITE_LEVEL));
            mImageWhiteLevelBox->set_units("nits");
            mImageWhiteLevelBox->set_tooltip(
                "tev's best guess of the image's reference white level (aka. paper white) in nits (cd/m²). "
                "This value represents the brightness a pixel value of 1.0 is meant to represent.\n\n"

                "tev usually has to guess this value for multiple reasons. "
                "Many image formats are display-referred and, as such, have no white level in (absolute) nits. "
                "Other formats are scene-referred and thus do have an absolute white level, but this information is often not stored in the file. "
                "Sometimes, it is not even clear whether a given image format is display- or scene-referred.\n\n"

                "However, when an image has unambiguous metadata, e.g. uses the PQ transfer function, "
                "tev can determine the white level reliably."
            );

            mImageWhiteLevelBox->set_editable(false);
            mImageWhiteLevelBox->set_enabled(false);
        }

        mColorsPopupButton = new PopupButton{buttonContainer, "Colors"};
        mColorsPopupButton->set_font_size(15);
        mColorsPopupButton->set_chevron_icon(0);
        mColorsPopupButton->set_tooltip("Color settings");

        // Color settings popup
        {
            auto popup = mColorsPopupButton->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            auto label = new Label{popup, "Inspection color space", "sans-bold", 20};
            label->set_tooltip(
                "The color space used for pixel inspection, i.e. the pixel values shown on hover, when zooming in, and in the histogram.\n\n"
                "IMPORTANT: this setting does NOT affect the appearance of the image shown on screen. "
                "The image is always displayed in correct colors; tev negotiates the correct display color space with the operating system automatically."
            );

            addSpacer(popup, 10);

            auto xy = new Widget{popup};
            xy->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 0, 2});

            label = new Label{xy, "Transfer"};
            label->set_fixed_width(100);
            label = new Label{xy, "Primaries"};
            label->set_fixed_width(100);

            vector<string> transferNames;
            for (const auto& t : TRANSFERS) {
                transferNames.emplace_back(t.second);
            }

            mInspectionTransferComboBox = new ComboBox{xy, transferNames};
            mInspectionTransferComboBox->set_font_size(16);
            mInspectionTransferComboBox->set_callback([this](int value) {
                TEV_ASSERT(value >= 0 && (size_t)value < TRANSFERS.size(), "Invalid transfer function index");
                setInspectionTransfer(ituth273::ETransfer(TRANSFERS[value].first));
            });

            vector<string> primariesNames;
            for (const auto& p : PRIMARIES) {
                primariesNames.emplace_back(p.second);
            }

            primariesNames.emplace_back("Custom");

            mInspectionPrimariesComboBox = new ComboBox{xy, primariesNames};
            mInspectionPrimariesComboBox->set_font_size(16);

            const auto makeChromaBox = [this](Widget* parent, size_t idx) {
                auto box = new FloatBox<float>{parent};
                box->set_editable(true);
                box->set_enabled(true);
                box->set_value_increment(0.0001f);
                box->number_format("%.05f");

                box->set_callback([this, idx](float val) {
                    TEV_ASSERT(idx < 8, "Invalid chromaticity index");

                    chroma_t chr = inspectionChroma();
                    chr[idx / 2][idx % 2] = val;
                    setInspectionChroma(chr);
                });

                return box;
            };

            addSpacer(xy, 6);
            addSpacer(xy, 6);

            const array<string_view, 4> labels = {"Red", "Green", "Blue", "White"};
            for (size_t i = 0; i < labels.size(); ++i) {
                new Label{xy, fmt::format("{} X", labels[i])};
                new Label{xy, fmt::format("{} Y", labels[i])};
                mInspectionPrimariesBoxes.emplace_back(makeChromaBox(xy, i * 2 + 0));
                mInspectionPrimariesBoxes.emplace_back(makeChromaBox(xy, i * 2 + 1));
                addSpacer(xy, 1);
                addSpacer(xy, 1);
            }

            mInspectionPrimariesComboBox->set_callback([this](int value) {
                TEV_ASSERT(value >= 0 && (size_t)value < PRIMARIES.size() + 1, "Invalid primaries index");

                if ((size_t)value >= PRIMARIES.size()) {
                    // When the user selects "Custom", we do not change the current chromaticities.
                    return;
                }

                setInspectionChroma(chroma(PRIMARIES[value].first));
            });

            addSpacer(xy, 1);
            addSpacer(xy, 1);

            mInspectionAdaptWhitePointButton = new Button{xy, "Adapt white"};
            mInspectionAdaptWhitePointButton->set_font_size(16);
            mInspectionAdaptWhitePointButton->set_flags(Button::ToggleButton);
            mInspectionAdaptWhitePointButton->set_tooltip(
                "Adapt from tev's internal D65 illuminant to the white point of the inspection color space using Bradford's algorithm. "
                "Enabling this feature is equivalent to a \"relative colorimetric\" color space conversion. Disabled is \"absolute colorimetric\"."
            );
            mInspectionAdaptWhitePointButton->set_change_callback([this](bool value) { setInspectionAdaptWhitePoint(value); });

            mInspectionPremultipliedAlphaButton = new Button{xy, "Premult. alpha"};
            mInspectionPremultipliedAlphaButton->set_font_size(16);
            mInspectionPremultipliedAlphaButton->set_flags(Button::ToggleButton);
            mInspectionPremultipliedAlphaButton->set_tooltip("Whether the inspected pixel values should have alpha premultiplied or not.");
            mInspectionPremultipliedAlphaButton->set_change_callback([this](bool value) { setInspectionPremultipliedAlpha(value); });

            setInspectionChroma(mImageCanvas->inspectionChroma());
            setInspectionTransfer(mImageCanvas->inspectionTransfer());
            setInspectionAdaptWhitePoint(mImageCanvas->inspectionAdaptWhitePoint());
            setInspectionPremultipliedAlpha(mImageCanvas->inspectionPremultipliedAlpha());

            addSpacer(popup, 20);

            new Label{popup, "Background color", "sans-bold", 20};
            mBackgroundColorWheel = new ColorWheel{popup};
            mBackgroundColorWheel->set_callback([this](const Color& value) {
                const float a = mBackgroundAlphaSlider->value();
                mImageCanvas->setBackgroundColor(Color{value.r() * a, value.g() * a, value.b() * a, a});
            });

            new Label{popup, "Background alpha"};
            mBackgroundAlphaSlider = new Slider{popup};
            mBackgroundAlphaSlider->set_range({0.0f, 1.0f});
            mBackgroundAlphaSlider->set_callback([this](float a) {
                const auto col = mBackgroundColorWheel->color();
                mImageCanvas->setBackgroundColor(Color{col.r() * a, col.g() * a, col.b() * a, a});
            });

            setBackgroundColorStraight(Color{0, 0, 0, 0});
        }
    }

    // Tonemap options
    {
        mTonemapButtonContainer = new Widget{mSidebarLayout};
        mTonemapButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

        auto makeTonemapButton = [&](string_view name, function<void()> callback) {
            auto button = new Button{mTonemapButtonContainer, name};
            button->set_flags(Button::RadioButton);
            button->set_font_size(15);
            button->set_callback(callback);
            return button;
        };

        makeTonemapButton("None", [this]() { setTonemap(ETonemap::SRGB); });
        makeTonemapButton("Gamma", [this]() { setTonemap(ETonemap::Gamma); });
        makeTonemapButton("FC", [this]() { setTonemap(ETonemap::FalseColor); });
        makeTonemapButton("+/-", [this]() { setTonemap(ETonemap::PositiveNegative); });

        setTonemap(ETonemap::SRGB);

        mTonemapButtonContainer->set_tooltip(
            "Tonemap selection:\n\n"

            "None\n"
            "No tonemapping\n\n"

            "Gamma\n"
            "Gamma correction + inverse sRGB\n"
            "Needed when displaying SDR to\n"
            "gamma-encoded displays.\n\n"

            "FC\n"
            "False-color visualization\n\n"

            "+/-\n"
            "Positive=Green, Negative=Red"
        );
    }

    // Error metrics
    {
        mMetricButtonContainer = new Widget{mSidebarLayout};
        mMetricButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

        auto makeMetricButton = [&](string_view name, function<void()> callback) {
            auto button = new Button{mMetricButtonContainer, name};
            button->set_flags(Button::RadioButton);
            button->set_font_size(15);
            button->set_callback(callback);
            return button;
        };

        makeMetricButton("E", [this]() { setMetric(EMetric::Error); });
        makeMetricButton("AE", [this]() { setMetric(EMetric::AbsoluteError); });
        makeMetricButton("SE", [this]() { setMetric(EMetric::SquaredError); });
        makeMetricButton("RAE", [this]() { setMetric(EMetric::RelativeAbsoluteError); });
        makeMetricButton("RSE", [this]() { setMetric(EMetric::RelativeSquaredError); });

        setMetric(EMetric::AbsoluteError);

        mMetricButtonContainer->set_tooltip(
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
        spacer->set_height(10);

        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
            auto label = new Label{panel, "Images", "sans-bold", 25};
            label->set_tooltip(
                "Select images either by left-clicking on them or by pressing arrow/number keys on your keyboard.\n"
                "Right-clicking an image marks it as the 'reference' image. "
                "While a reference image is set, the currently selected image is not simply displayed, but compared to the reference image."
            );
        }

        // Histogram of selected image
        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mHistogram = new MultiGraph{panel, ""};
        }

        // Fuzzy filter of open images
        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

            mFilter = new TextBox{panel, ""};
            mFilter->set_editable(true);
            mFilter->set_alignment(TextBox::Alignment::Left);
            mFilter->set_callback([this](string_view filter) { return setFilter(filter); });

            mFilter->set_placeholder("Find");
            mFilter->set_tooltip(
                fmt::format(
                    "Filters visible images and channel groups according to a supplied string. "
                    "The string must have the format 'image:group'. "
                    "Only images whose name contains 'image' and groups whose name contains 'group' will be visible.\n\n"
                    "Keyboard shortcut:\n{}+F",
                    HelpWindow::COMMAND
                )
            );

            mRegexButton = new Button{panel, "", FA_SEARCH};
            mRegexButton->set_tooltip("Treat filter as regular expression");
            mRegexButton->set_pushed(false);
            mRegexButton->set_flags(Button::ToggleButton);
            mRegexButton->set_font_size(15);
            mRegexButton->set_change_callback([this](bool value) { setUseRegex(value); });
        }

        // Playback controls
        {
            auto playback = new Widget{mSidebarLayout};
            playback->set_layout(new GridLayout{Orientation::Horizontal, 6, Alignment::Fill, 5, 2});

            auto makePlaybackButton = [&](string_view name, bool enabled, function<void()> callback, int icon = 0, string_view tooltip = "") {
                auto button = new Button{playback, name, icon};
                button->set_callback(callback);
                button->set_tooltip(tooltip);
                button->set_font_size(15);
                button->set_enabled(enabled);
                button->set_padding({10, 10});
                return button;
            };

            mPlayButton = makePlaybackButton("", true, [] {}, FA_PLAY, "Play (Space)");
            mPlayButton->set_flags(Button::ToggleButton);
            mPlayButton->set_change_callback([this](bool value) { setPlayingBack(value); });

            mAnyImageButtons.push_back(
                makePlaybackButton("", false, [this] { selectImage(nthVisibleImage(0)); }, FA_FAST_BACKWARD, "Front (Home)")
            );

            mAnyImageButtons.push_back(
                makePlaybackButton("", false, [this] { selectImage(nthVisibleImage(mImages.size())); }, FA_FAST_FORWARD, "Back (End)")
            );

            mFpsTextBox = new IntBox<int>{playback, 24};
            mFpsTextBox->set_default_value("24");
            mFpsTextBox->set_units("fps");
            mFpsTextBox->set_editable(true);
            mFpsTextBox->set_alignment(TextBox::Alignment::Right);
            mFpsTextBox->set_min_max_values(1, 1000);
            mFpsTextBox->set_spinnable(true);

            mAutoFitToScreenButton =
                makePlaybackButton("", true, {}, FA_EXPAND_ARROWS_ALT, "Automatically fit image to screen upon selection.");
            mAutoFitToScreenButton->set_flags(Button::Flags::ToggleButton);
            mAutoFitToScreenButton->set_change_callback([this](bool value) { setAutoFitToScreen(value); });

            mResizeWindowToFitImageOnLoadButton =
                makePlaybackButton("", true, {}, FA_WINDOW_RESTORE, "Automatically resize tev's window to fit image on load.");
            mResizeWindowToFitImageOnLoadButton->set_flags(Button::Flags::ToggleButton);
            mResizeWindowToFitImageOnLoadButton->set_change_callback([this](bool value) { setResizeWindowToFitImageOnLoad(value); });
            mResizeWindowToFitImageOnLoadButton->set_pushed(true);
        }

        // Save, refresh, load, close
        {
            auto tools = new Widget{mSidebarLayout};
            tools->set_layout(new GridLayout{Orientation::Horizontal, 7, Alignment::Fill, 5, 1});

            auto makeImageButton = [&](string_view name, bool enabled, function<void()> callback, int icon = 0, string_view tooltip = "") {
                auto button = new Button{tools, name, icon};
                button->set_callback(callback);
                button->set_tooltip(tooltip);
                button->set_font_size(15);
                button->set_enabled(enabled);
                button->set_padding({10, 10});
                return button;
            };

            makeImageButton("", true, [this] { openImageDialog(); }, FA_FOLDER, fmt::format("Open ({}+O)", HelpWindow::COMMAND));

            mCurrentImageButtons.push_back(
                makeImageButton("", false, [this] { saveImageDialog(); }, FA_SAVE, fmt::format("Save ({}+S)", HelpWindow::COMMAND))
            );

            mCurrentImageButtons.push_back(makeImageButton(
                "", false, [this] { reloadImage(mCurrentImage); }, FA_RECYCLE, fmt::format("Reload ({}+R or F5)", HelpWindow::COMMAND)
            ));

            mAnyImageButtons.push_back(makeImageButton(
                "A", false, [this] { reloadAllImages(); }, 0, fmt::format("Reload all ({}+Shift+R or {}+F5)", HelpWindow::COMMAND, HelpWindow::COMMAND)
            ));

            mWatchFilesForChangesButton =
                makeImageButton("W", true, {}, 0, "Watch image files and directories for changes and reload them automatically.");
            mWatchFilesForChangesButton->set_flags(Button::Flags::ToggleButton);
            mWatchFilesForChangesButton->set_change_callback([this](bool value) { setWatchFilesForChanges(value); });

            mImageInfoButton = makeImageButton("", false, {}, FA_INFO, "Show image info and metadata (I)");
            mImageInfoButton->set_flags(Button::ToggleButton);
            mImageInfoButton->set_change_callback([this](bool) { toggleImageInfoWindow(); });
            mAnyImageButtons.push_back(mImageInfoButton);

            mCurrentImageButtons.push_back(makeImageButton(
                "",
                false,
                [this] {
                    auto* glfwWindow = screen()->glfw_window();
                    // There is no explicit access to the currently pressed modifier keys here, so we need to directly ask GLFW. In case
                    // this is needed more often, it may be worth inheriting Button and overriding mouse_button_event (similar to
                    // ImageButton).
                    if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                        removeAllImages();
                    } else {
                        removeImage(mCurrentImage);
                    }
                },
                FA_TIMES,
                fmt::format("Close ({}+W); Close all ({}+Shift+W)", HelpWindow::COMMAND, HelpWindow::COMMAND)
            ));

            spacer = new Widget{mSidebarLayout};
            spacer->set_height(3);
        }

        // List of open images
        {
            mImageScrollContainer = new VScrollPanel{mSidebarLayout};
            mImageScrollContainer->set_fixed_width(mSidebarLayout->fixed_width());

            mScrollContent = new Widget{mImageScrollContainer};
            mScrollContent->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

            mImageButtonContainer = new Widget{mScrollContent};
            mImageButtonContainer->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
        }
    }

    // Group selection
    {
        mFooter = new Widget{mVerticalScreenSplit};

        mGroupButtonContainer = new Widget{mFooter};
        mGroupButtonContainer->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});
        mGroupButtonContainer->set_fixed_height(25);
        mFooter->set_fixed_height(25);
        mFooter->set_visible(false);
    }

    set_resize_callback([this](Vector2i) { requestLayoutUpdate(); });
    resize_callback_event(m_size.x(), m_size.y()); // Required on some OSs to get up-to-date pixel ratio

    selectImage(nullptr);
    selectReference(nullptr);

    if (!maximize) {
        // mDidFitToImage is only used when starting out maximized and wanting to fit the window to the image size after *unmaximizing*.
        mDidFitToImage = 3;
    }

    updateColorCapabilities();
    updateLayout();

    mInitialized = true;
}

bool ImageViewer::resize_event(const Vector2i& size) {
    mImageCanvas->setPixelRatio(pixel_ratio());
    requestLayoutUpdate();

    return Screen::resize_event(size);
}

bool ImageViewer::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    // Check if the user performed mousedown on an imagebutton so we can mark it as being dragged. This has to occur before
    // Screen::mouse_button_event as the button would absorb the event.
    if (down) {
        if (mImageScrollContainer->contains(p - mSidebarLayout->parent()->position())) {
            auto& buttons = mImageButtonContainer->children();

            Vector2i relMousePos = (absolute_position() + p) - mImageButtonContainer->absolute_position();

            for (size_t i = 0; i < buttons.size(); ++i) {
                const auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos) && !imgButton->textBoxVisible()) {
                    mDraggingStartPosition = relMousePos - imgButton->position();
                    mDragType = EMouseDragType::ImageButtonDrag;
                    mDraggedImageButtonId = i;
                    break;
                }
            }
        }
    }

    if (Screen::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    // Hide caption textbox when the user performed mousedown on any other component
    if (down) {
        for (auto& b : mImageButtonContainer->children()) {
            dynamic_cast<ImageButton*>(b)->hideTextBox();
        }
    }

    auto* glfwWindow = screen()->glfw_window();
    if (down) {
        if (mDragType != EMouseDragType::ImageButtonDrag) {
            mDraggingStartPosition = p;
            if (canDragSidebarFrom(p)) {
                mDragType = EMouseDragType::SidebarDrag;
                return true;
            } else if (mImageCanvas->contains(p) && mCurrentImage) {
                mDragType = glfwGetKey(glfwWindow, GLFW_KEY_C) ? EMouseDragType::ImageCrop : EMouseDragType::ImageDrag;
                return true;
            }
        }
    } else {
        if (mDragType == EMouseDragType::ImageButtonDrag) {
            requestLayoutUpdate();
        } else if (mDragType == EMouseDragType::ImageCrop) {
            if (norm(mDraggingStartPosition - p) < CROP_MIN_SIZE) {
                // If the user did not drag the mouse far enough, we assume that they wanted to reset the crop rather than create a new one.
                mImageCanvas->setCrop(nullopt);
            }
        }

        mDragType = EMouseDragType::None;
    }

    return true;
}

bool ImageViewer::mouse_motion_event_f(const Vector2f& p, const Vector2f& rel, int button, int modifiers) {
    if (Screen::mouse_motion_event_f(p, rel, button, modifiers)) {
        return true;
    }

    bool shouldShowResizeCursor = mDragType == EMouseDragType::SidebarDrag || canDragSidebarFrom(p);
    Cursor cursorType = shouldShowResizeCursor ? Cursor::HResize : Cursor::Arrow;

    mSidebarLayout->set_cursor(cursorType);
    mImageCanvas->set_cursor(cursorType);

    switch (mDragType) {
        case EMouseDragType::SidebarDrag:
            mSidebar->set_fixed_width(clamp(p.x(), (float)SIDEBAR_MIN_WIDTH, (float)m_size.x() - 10.0f));
            requestLayoutUpdate();
            break;

        case EMouseDragType::ImageDrag: {
            Vector2f relativeMovement = rel;
            auto* glfwWindow = screen()->glfw_window();
            // There is no explicit access to the currently pressed modifier keys here, so we need to directly ask GLFW.
            if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                relativeMovement /= 8;
            } else if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL)) {
                relativeMovement *= 8;
            }

            // If left mouse button is held, move the image with mouse movement
            if ((button & 1) != 0) {
                mImageCanvas->translate(relativeMovement);
            }

            // If middle mouse button is held, zoom in-out with up-down mouse movement
            if ((button & 4) != 0) {
                mImageCanvas->scale(relativeMovement.y() / 8.0f, Vector2f{mDraggingStartPosition});
            }

            break;
        }

        case EMouseDragType::ImageCrop: {
            Vector2i relStartMousePos = (absolute_position() + mDraggingStartPosition) - mImageCanvas->absolute_position();
            Vector2i relMousePos = (absolute_position() + Vector2i{p}) - mImageCanvas->absolute_position();

            // Require a minimum movement to start cropping. Since this is measured in nanogui / screen space and not image space, this does
            // not prevent the cropping of smaller image regions. Just zoom in before cropping smaller regions.
            if (norm(relStartMousePos - relMousePos) < CROP_MIN_SIZE) {
                return false;
            }

            auto startImageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relStartMousePos);
            auto imageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relMousePos);

            // sanitize the input crop
            Box2i crop = {{{startImageCoords, imageCoords}}};
            crop.max += Vector2i{1};

            // we do not need to worry about min/max ordering here, as setCrop sanitizes the input for us
            mImageCanvas->setCrop(crop);

            break;
        }

        case EMouseDragType::ImageButtonDrag: {
            auto& buttons = mImageButtonContainer->children();
            Vector2i relMousePos = (absolute_position() + Vector2i{p}) - mImageButtonContainer->absolute_position();

            TEV_ASSERT(mDraggedImageButtonId < buttons.size(), "Dragged image button id is out of bounds.");
            auto* draggedImgButton = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId]);
            for (size_t i = 0; i < buttons.size(); ++i) {
                if (i == mDraggedImageButtonId) {
                    continue;
                }

                auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos)) {
                    Vector2i pos = imgButton->position();
                    pos.y() += ((int)draggedImgButton->id() - (int)imgButton->id()) * imgButton->size().y();
                    imgButton->set_position(pos);
                    imgButton->mouse_enter_event(relMousePos, false);

                    moveImageInList(mDraggedImageButtonId, i);
                    mDraggedImageButtonId = i;
                    break;
                }
            }

            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->set_position(relMousePos - mDraggingStartPosition);

            break;
        }

        case EMouseDragType::None: break;
    }

    return focused();
}

bool ImageViewer::drop_event(const vector<string>& filenames) {
    if (Screen::drop_event(filenames)) {
        return true;
    }

    for (size_t i = 0; i < filenames.size(); ++i) {
        mImagesLoader->enqueue(toPath(filenames[i]), "", i == filenames.size() - 1);
    }

    // Make sure we gain focus after dragging files into here.
    focusWindow();
    return true;
}

bool ImageViewer::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    int numGroups = mGroupButtonContainer->child_count();

    // Keybindings which should _not_ respond to repeats
    if (action == GLFW_PRESS) {
        // The checks for mod + GLFW_KEY_0 and GLFW_KEY_9 need to happen prior to checking for generic number keys as they should take
        // priority over group switching on Windows/Linux. No conflics on macOS.
        if (key == GLFW_KEY_0 && (modifiers & SYSTEM_COMMAND_MOD)) {
            mImageCanvas->resetTransform();
            return true;
        } else if (key == GLFW_KEY_F && (modifiers & SYSTEM_COMMAND_MOD)) {
            mFilter->request_focus();
            mFilter->select_all();
            return true;
        } else if (key == GLFW_KEY_F || (key == GLFW_KEY_9 && (modifiers & SYSTEM_COMMAND_MOD))) {
            if (mCurrentImage) {
                mImageCanvas->fitImageToScreen(*mCurrentImage);
            }
            return true;
        } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
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
#ifdef __APPLE__
        } else if (key == GLFW_KEY_ENTER) {
#else
        } else if (key == GLFW_KEY_F2) {
#endif
            const auto id = imageId(mCurrentImage);
            if (id.has_value()) {
                dynamic_cast<ImageButton*>(mImageButtonContainer->child_at(*id))->showTextBox();
                requestLayoutUpdate();
            }

            return true;
        } else if (key == GLFW_KEY_N) {
            normalizeExposureAndOffset();
            return true;
        } else if (key == GLFW_KEY_U) {
            mClipToLdrButton->set_pushed(!mClipToLdrButton->pushed());
            mImageCanvas->setClipToLdr(mClipToLdrButton->pushed());
            return true;
        } else if (key == GLFW_KEY_I) {
            toggleImageInfoWindow();
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
        } else if (key == GLFW_KEY_X) {
            // X for "eXplode channels
            if (mCurrentImage) {
                mCurrentImage->decomposeChannelGroup(mCurrentGroup);

                // Resets channel group buttons to include the now exploded channels
                selectImage(mCurrentImage);
            }

            if (mCurrentReference) {
                mCurrentReference->decomposeChannelGroup(mCurrentGroup);
                selectReference(mCurrentReference);
            }
        } else if (key == GLFW_KEY_B && (modifiers & SYSTEM_COMMAND_MOD)) {
            setUiVisible(!isUiVisible());
            return true;
        } else if (key == GLFW_KEY_O && (modifiers & SYSTEM_COMMAND_MOD)) {
            openImageDialog();
            return true;
        } else if (key == GLFW_KEY_S && (modifiers & SYSTEM_COMMAND_MOD)) {
            saveImageDialog();
            return true;
        } else if (
            // question mark on US layout
            key == GLFW_KEY_SLASH && (modifiers & GLFW_MOD_SHIFT)
        ) {
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
            setPlayingBack(!playingBack());
            return true;
        } else if (key == GLFW_KEY_ESCAPE) {
            setFilter("");
            return true;
        } else if (key == GLFW_KEY_Q && (modifiers & SYSTEM_COMMAND_MOD)) {
            set_visible(false);
            return true;
        } else if (key == GLFW_KEY_C && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                try {
                    copyImageNameToClipboard();
                } catch (const runtime_error& e) { showErrorDialog(fmt::format("Failed to copy image name to clipboard: {}", e.what())); }
            } else {
                try {
                    copyImageCanvasToClipboard();
                } catch (const runtime_error& e) { showErrorDialog(fmt::format("Failed to copy image to clipboard: {}", e.what())); }
            }

            return true;
        } else if (key == GLFW_KEY_V && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                const char* clipboardString = glfwGetClipboardString(m_glfw_window);
                if (clipboardString) {
                    tlog::warning() << fmt::format(
                        "Pasted string \"{}\" from clipboard, but tev can only paste images from clipboard.", clipboardString
                    );
                }
            } else {
                try {
                    pasteImagesFromClipboard();
                } catch (const runtime_error& e) { showErrorDialog(fmt::format("Failed to paste image from clipboard: {}", e.what())); }
            }

            return true;
        }
    }

    // Keybindings which should respond to repeats
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_KP_ADD || key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
            float scaleAmount = 1.0f;
            if (modifiers & GLFW_MOD_SHIFT) {
                scaleAmount /= 8;
            } else if (modifiers & GLFW_MOD_CONTROL) {
                scaleAmount *= 8;
            }

            if (key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
                scaleAmount = -scaleAmount;
            }

            Vector2f origin = Vector2f{mImageCanvas->position()} + Vector2f{mImageCanvas->size()} * 0.5f;

            mImageCanvas->scale(scaleAmount, {origin.x(), origin.y()});
            return true;
        }

        if (key == GLFW_KEY_E) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setExposure(exposure() - 0.5f);
            } else {
                setExposure(exposure() + 0.5f);
            }

            return true;
        }

        if (key == GLFW_KEY_O) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setOffset(offset() - 0.1f);
            } else {
                setOffset(offset() + 0.1f);
            }

            return true;
        }

        if (key == GLFW_KEY_G) {
            if (mGammaSlider->enabled()) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    setGamma(gamma() - 0.1f);
                } else {
                    setGamma(gamma() + 0.1f);
                }
            }

            return true;
        }

        if (key == GLFW_KEY_W && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                removeAllImages();
            } else {
                removeImage(mCurrentImage);
            }

            return true;
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_W || key == GLFW_KEY_PAGE_UP ||
                   (key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL) && (modifiers & GLFW_MOD_SHIFT))) {
            if (key != GLFW_KEY_TAB && (modifiers & GLFW_MOD_SHIFT)) {
                selectReference(nextImage(mCurrentReference, Backward));
            } else {
                selectImage(nextImage(mCurrentImage, Backward));
            }

            return true;
        } else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S || key == GLFW_KEY_PAGE_DOWN ||
                   (key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL) && !(modifiers & GLFW_MOD_SHIFT))) {
            if (key != GLFW_KEY_TAB && (modifiers & GLFW_MOD_SHIFT)) {
                selectReference(nextImage(mCurrentReference, Forward));
            } else {
                selectImage(nextImage(mCurrentImage, Forward));
            }

            return true;
        }

        if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D || key == GLFW_KEY_RIGHT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>(((int)tonemap() + 1) % (int)ETonemap::Count));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>(((int)metric() + 1) % (int)EMetric::Count));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Forward));
            }

            return true;
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A || key == GLFW_KEY_LEFT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>(((int)tonemap() - 1 + (int)ETonemap::Count) % (int)ETonemap::Count));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>(((int)metric() - 1 + (int)EMetric::Count) % (int)EMetric::Count));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Backward));
            }

            return true;
        }

        float translationAmount = 64.0f;
        if (modifiers & GLFW_MOD_SHIFT) {
            translationAmount /= 8.0f;
            if (modifiers & GLFW_MOD_CONTROL) {
                translationAmount /= 8.0f;
            }
        } else if (modifiers & GLFW_MOD_CONTROL) {
            translationAmount *= 8.0f;
        }

        if (key == GLFW_KEY_H) {
            mImageCanvas->translate({translationAmount, 0});
            return true;
        } else if (key == GLFW_KEY_L) {
            mImageCanvas->translate({-translationAmount, 0});
            return true;
        } else if (key == GLFW_KEY_J) {
            mImageCanvas->translate({0, -translationAmount});
            return true;
        } else if (key == GLFW_KEY_K) {
            mImageCanvas->translate({0, translationAmount});
            return true;
        }
    }

    return true;
}

void ImageViewer::focusWindow() { glfwFocusWindow(m_glfw_window); }

void ImageViewer::draw_contents() {
    if (!mInitialized) {
        return;
    }

    updateColorCapabilities();

    // Update SDR white level from system settings if not overridden by the user
    if (displayWhiteLevelSetting() == EDisplayWhiteLevelSetting::System) {
        mDisplayWhiteLevelBox->set_value(glfwGetWindowSdrWhiteLevel(m_glfw_window));
    }

    updateCurrentMonitorSize();

    // HACK HACK HACK: on Windows, when restoring a window from maximization, the old window size is restored _several times_, necessitating
    // a repeated resize to the actually desired window size.
    if (mDidFitToImage < 3 && !isMaximized()) {
        ++mDidFitToImage;

        if (resizeWindowToFitImageOnLoad()) {
            resizeToFit(sizeToFitAllImages());
        }
    }

    clear();

    // If playing back, ensure correct frame pacing
    if (playingBack() && mTaskQueue.empty()) {
        auto fps = clamp(mFpsTextBox->value(), 1, 1000);
        auto seconds_per_frame = chrono::duration<float>{1.0f / fps};
        auto now = chrono::steady_clock::now();

        if (now - mLastPlaybackFrameTime > 500s) {
            // If lagging behind too far, drop the frames, but otherwise...
            mLastPlaybackFrameTime = now;
            selectImage(nextImage(mCurrentImage, Forward), false);
        } else {
            // ...advance by as many frames as the user-specified FPS would demand, given the elapsed time since the last render.
            while (now - mLastPlaybackFrameTime >= seconds_per_frame) {
                mLastPlaybackFrameTime += chrono::duration_cast<chrono::steady_clock::duration>(seconds_per_frame);
                selectImage(nextImage(mCurrentImage, Forward), false);
            }
        }
    }

    // If watching files for changes, do so every 100ms
    if (watchFilesForChanges()) {
        auto now = chrono::steady_clock::now();
        if (now - mLastFileChangesCheckTime >= 100ms) {
            reloadImagesWhoseFileChanged();
            mImagesLoader->checkDirectoriesForNewFilesAndLoadThose();
            mLastFileChangesCheckTime = now;
        }
    }

    // In case any images got loaded in the background, they sit around in mImagesLoader. Here is the place where we actually add them to
    // the GUI. Focus the application in case one of the new images is meant to override the current selection.
    bool newFocus = false;
    while (auto addition = mImagesLoader->tryPop()) {
        newFocus |= addition->shallSelect;

        bool first = true;
        for (auto& image : addition->images) {
            // If the loaded file consists of multiple images (such as multi-part EXRs), select the first part if selection is desired.
            bool shallSelect = first ? addition->shallSelect : false;
            if (addition->toReplace) {
                replaceImage(addition->toReplace, image, shallSelect);
            } else {
                addImage(image, shallSelect);
            }
            first = false;
        }
    }

    if (newFocus) {
        focusWindow();
    }

    // mTaskQueue contains jobs that should be executed on the main thread. It is useful for handling callbacks from background threads
    while (auto task = mTaskQueue.tryPop()) {
        (*task)();
    }

    for (auto it = begin(mToBump); it != end(mToBump);) {
        auto& image = *it;
        bool isShown = image == mCurrentImage || image == mCurrentReference;

        // If the image is no longer shown, bump ID immediately. Otherwise, wait until canvas statistics were ready for over 200 ms.
        if (!isShown || chrono::steady_clock::now() - mImageCanvas->canvasStatistics()->becameReadyAt() > 200ms) {
            image->bumpId();
            auto localIt = it;
            ++it;
            mToBump.erase(localIt);
        } else {
            ++it;
        }
    }

    if (mRequiresFilterUpdate) {
        updateFilter();
        mRequiresFilterUpdate = false;
    }

    bool anyImageVisible = mCurrentImage || mCurrentReference ||
        any_of(begin(mImageButtonContainer->children()), end(mImageButtonContainer->children()), [](const auto& c) { return c->visible(); });

    for (auto button : mAnyImageButtons) {
        button->set_enabled(anyImageVisible);
    }

    if (mRequiresLayoutUpdate) {
        Vector2i oldDraggedImageButtonPos{0, 0};
        auto& buttons = mImageButtonContainer->children();
        if (mDragType == EMouseDragType::ImageButtonDrag) {
            oldDraggedImageButtonPos = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->position();
        }

        updateLayout();
        mRequiresLayoutUpdate = false;

        if (mDragType == EMouseDragType::ImageButtonDrag) {
            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->set_position(oldDraggedImageButtonPos);
        }
    }

    updateTitle();

    // Update histogram
    static const string histogramTooltipBase =
        "Histogram of color values with logarithmic x-axis. Adapts to the currently chosen channel group, error metric, and inspection color space.";
    auto lazyCanvasStatistics = mImageCanvas->canvasStatistics();
    if (lazyCanvasStatistics) {
        if (lazyCanvasStatistics->isReady()) {
            auto statistics = lazyCanvasStatistics->get();
            mHistogram->setNChannels(statistics->nChannels);
            mHistogram->setColors(statistics->histogramColors);
            mHistogram->setValues(statistics->histogram);
            mHistogram->setMinimum(statistics->minimum);
            mHistogram->setMean(statistics->mean);
            mHistogram->setMaximum(statistics->maximum);
            mHistogram->setZero(statistics->histogramZero);
            mHistogram->set_tooltip(
                fmt::format(
                    "{}\n\n"
                    "Minimum: {:.3f}\n"
                    "Mean: {:.3f}\n"
                    "Maximum: {:.3f}",
                    histogramTooltipBase,
                    statistics->minimum,
                    statistics->mean,
                    statistics->maximum
                )
            );
        }
    } else {
        mHistogram->setNChannels(1);
        mHistogram->setColors({
            {1.0f, 1.0f, 1.0f}
        });
        mHistogram->setValues({{0.0f}});
        mHistogram->setMinimum(0);
        mHistogram->setMean(0);
        mHistogram->setMaximum(0);
        mHistogram->setZero(0);
        mHistogram->set_tooltip(fmt::format("{}", histogramTooltipBase));
    }
}

void ImageViewer::updateColorCapabilities() {
    const auto prevColorSpace = mSystemColorSpace;
    mSystemColorSpace = ColorSpace{
        .transfer = ituth273::fromWpTransfer(glfwGetWindowTransfer(m_glfw_window)),
        .primaries = static_cast<EWpPrimaries>(glfwGetWindowPrimaries(m_glfw_window)),
        .maxLuminance = glfwGetWindowMaxLuminance(m_glfw_window),
    };

    if (mSystemColorSpace == prevColorSpace) {
        return;
    }

    const auto& cs = *mSystemColorSpace;

#if defined(__APPLE__)
    const auto [supportsWideGamut, supportsHdr] = test_10bit_edr_support();
    const bool supportsAbsoluteBrightness = false;
#else // Linux and Windows
    const bool supportsExtendedRange = m_float_buffer || cs.transfer == ituth273::ETransfer::PQ || cs.transfer == ituth273::ETransfer::HLG;
    const bool supportsHdr = supportsExtendedRange &&
        (cs.maxLuminance > 80.0f || cs.maxLuminance == 0.0f); // Some systems don't report max luminance (value of 0.0). Assume HDR then.
    const bool supportsWideGamut = supportsExtendedRange ||
        cs.primaries != EWpPrimaries::SRGB; // Non-sRGB primaries imply wide color support.
    const bool supportsAbsoluteBrightness = supportsHdr;
#endif

    tlog::info() << fmt::format(
        "{} {} bit {} point frame buffer with primaries={} transfer={} range={}",
        prevColorSpace ? "Switched to" : "Initialized",
        this->bits_per_sample(),
        m_float_buffer ? "floating" : "fixed",
        toString(cs.primaries),
        ituth273::toString(cs.transfer),
        supportsHdr           ? "hdr" :
            supportsWideGamut ? "wide_gamut_sdr" :
                                "sdr"
    );

    // Update UI elements accordingly
    mHdrPopupButton->set_enabled(supportsHdr);
    if (supportsHdr) {
        mHdrPopupButton->set_tooltip("HDR Settings");
    } else {
        mHdrPopupButton->set_tooltip(
            "Your system does not support HDR colors. "
            "Make sure that your OS, GPU, and display support HDR and that it is enabled in your system and display settings."
        );
    }

    mClipToLdrButton->set_enabled(supportsHdr);

    if (supportsAbsoluteBrightness) {
        mDisplayWhiteLevelBox->set_tooltip(
            "The display reference white level (aka. paper white) in nits (cd/m²). "
            "This value determines how bright a pixel value of 1.0 appears on the display. "
            "It follows your system settings by default.\n\n"
            "You can customize this value to change the brightness at which images are displayed. "
            "Or you can link this value to the image white level (if known) to display images at their absolute brightness "
            "rather than relative to your system's brightness setting."
        );
    } else {
        mDisplayWhiteLevelBox->set_tooltip(
            "Your system or display does not support absolute brightness rendering. "
            "White level override is disabled."
        );
    }

    mDisplayWhiteLevelBox->set_editable(supportsAbsoluteBrightness);
    mDisplayWhiteLevelBox->set_enabled(supportsAbsoluteBrightness);

    mDisplayWhiteLevelSettingComboBox->set_enabled(supportsAbsoluteBrightness);
}

void ImageViewer::insertImage(shared_ptr<Image> image, size_t index, bool shallSelect) {
    if (!image) {
        throw invalid_argument{"Image may not be null."};
    }

    if (mDragType == EMouseDragType::ImageButtonDrag && index <= mDraggedImageButtonId) {
        ++mDraggedImageButtonId;
    }

    auto button = new ImageButton{nullptr, image->name(), true};
    button->set_font_size(15);
    button->setId(index + 1);
    button->set_tooltip(image->toString());

    button->setSelectedCallback([this, image]() { selectImage(image); });

    button->setReferenceCallback([this, image](bool isReference) {
        if (!isReference) {
            selectReference(nullptr);
        } else {
            selectReference(image);
        }
    });

    button->setCaptionChangeCallback([this]() { mRequiresFilterUpdate = true; });

    mImageButtonContainer->add_child((int)index, button);
    mImages.insert(begin(mImages) + index, image);

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Ensure the new image button will have the correct visibility state.
    setFilter(mFilter->value());

    requestLayoutUpdate();

    // First image got added, let's select it.
    if ((index == 0 && mImages.size() == 1) || shallSelect) {
        selectImage(image);
        if (!isMaximized() && resizeWindowToFitImageOnLoad()) {
            resizeToFit(sizeToFitImage(image));
        }
    }
}

void ImageViewer::moveImageInList(size_t oldIndex, size_t newIndex) {
    if (oldIndex == newIndex) {
        return;
    }

    TEV_ASSERT(oldIndex < mImages.size(), "oldIndex must be smaller than the number of images.");
    TEV_ASSERT(newIndex < mImages.size(), "newIndex must be smaller than the number of images.");

    auto* button = dynamic_cast<ImageButton*>(mImageButtonContainer->child_at((int)oldIndex));
    TEV_ASSERT(button, "Image button must exist.");

    button->inc_ref();
    mImageButtonContainer->remove_child_at((int)oldIndex);
    mImageButtonContainer->add_child((int)newIndex, button);
    button->dec_ref();

    int change = newIndex > oldIndex ? 1 : -1;
    for (size_t i = oldIndex; i != newIndex; i += change) {
        auto* curButton = dynamic_cast<ImageButton*>(mImageButtonContainer->child_at((int)i));
        if (curButton->visible()) {
            curButton->setId(curButton->id() - change);
            button->setId(button->id() + change);
        }
    }

    auto img = mImages[oldIndex];
    mImages.erase(mImages.begin() + oldIndex);
    mImages.insert(mImages.begin() + newIndex, img);

    requestLayoutUpdate();
}

void ImageViewer::removeImage(shared_ptr<Image> image) {
    const auto id = imageId(image);
    if (!id) {
        return;
    }

    if (mDragType == EMouseDragType::ImageButtonDrag) {
        // If we're currently dragging the to-be-removed image, stop.
        if (id == mDraggedImageButtonId) {
            requestLayoutUpdate();
            mDragType = EMouseDragType::None;
        } else if (id < mDraggedImageButtonId) {
            --mDraggedImageButtonId;
        }
    }

    auto nextCandidate = nextImage(image, Forward);
    // If we rolled over, let's rather use the previous image. We don't want to jumpt to the beginning when deleting the last image in our
    // list.
    if (imageId(nextCandidate) < id) {
        nextCandidate = nextImage(image, Backward);
    }

    // If `nextImage` produced the same image again, this means that `image` is the only (visible) image and hence, after removal, should be
    // replaced by no selection at all.
    if (nextCandidate == image) {
        nextCandidate = nullptr;
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    request_focus();

    mImages.erase(begin(mImages) + *id);
    mImageButtonContainer->remove_child_at((int)*id);

    if (mImages.empty()) {
        selectImage(nullptr);
        selectReference(nullptr);
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
    if (mImages.empty()) {
        return;
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    request_focus();

    for (int i = (int)mImages.size() - 1; i >= 0; --i) {
        if (mImageButtonContainer->child_at(i)->visible()) {
            mImages.erase(begin(mImages) + i);
            mImageButtonContainer->remove_child_at(i);
        }
    }

    // No images left to select
    selectImage(nullptr);
    selectReference(nullptr);
}

void ImageViewer::replaceImage(shared_ptr<Image> image, shared_ptr<Image> replacement, bool shallSelect) {
    if (replacement == nullptr) {
        throw runtime_error{"Must not replace image with nullptr."};
    }

    const auto currentId = imageId(mCurrentImage);
    const auto id = imageId(image);
    if (!id) {
        addImage(replacement, shallSelect);
        return;
    }

    // Preserve image button caption when replacing an image
    ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children().at(*id));
    const string caption{ib->caption()};

    // If we already have the image selected, we must re-select it regardless of the `shallSelect` parameter.
    shallSelect |= currentId == id;

    const auto referenceId = imageId(mCurrentReference);

    removeImage(image);
    insertImage(replacement, *id, shallSelect);

    ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children().at(*id));
    ib->setCaption(caption);

    if (referenceId) {
        selectReference(mImages.at(*referenceId));
    }
}

void ImageViewer::reloadImage(shared_ptr<Image> image, bool shallSelect) {
    if (imageId(image)) {
        mImagesLoader->enqueue(image->path(), image->channelSelector(), shallSelect, image);
    }
}

void ImageViewer::reloadAllImages() {
    for (size_t i = 0; i < mImages.size(); ++i) {
        reloadImage(mImages[i]);
    }
}

void ImageViewer::reloadImagesWhoseFileChanged() {
    for (size_t i = 0; i < mImages.size(); ++i) {
        auto& image = mImages[i];
        if (!fs::exists(image->path())) {
            continue;
        }

        fs::file_time_type fileLastModified;

        // Unlikely, but the file could have been deleted, moved, or something else could have happened to it that makes obtaining its last
        // modified time impossible. Ignore such errors.
        try {
            fileLastModified = fs::last_write_time(image->path());
        } catch (...) { continue; }

        if (fileLastModified != image->fileLastModified()) {
            // Updating the last-modified date prevents double-scheduled reloads if the load take a lot of time or fails.
            image->setFileLastModified(fileLastModified);
            reloadImage(image);
        }
    }
}

void ImageViewer::updateImage(
    string_view imageName, bool shallSelect, string_view channel, int x, int y, int width, int height, span<const float> imageData
) {
    auto image = imageByName(imageName);
    if (!image) {
        tlog::warning() << "Image " << imageName << " could not be updated, because it does not exist.";
        return;
    }

    image->updateChannel(channel, x, y, width, height, imageData);
    if (shallSelect) {
        selectImage(image);
    }

    // This image needs newly computed statistics... so give it a new ID. However, if the image is currently shown, we don't want to
    // overwhelm the CPU, so we only launch new statistics computations every so often. These computations are scheduled from `drawContents`
    // via the `mToBump` set.
    if (image != mCurrentImage && image != mCurrentReference) {
        image->bumpId();
    } else {
        mToBump.insert(image);
    }
}

void ImageViewer::updateImageVectorGraphics(string_view imageName, bool shallSelect, bool append, span<const VgCommand> commands) {
    auto image = imageByName(imageName);
    if (!image) {
        tlog::warning() << "Vector graphics of image " << imageName << " could not be updated, because it does not exist.";
        return;
    }

    image->updateVectorGraphics(append, commands);
    if (shallSelect) {
        selectImage(image);
    }
}

void ImageViewer::selectImage(const shared_ptr<Image>& image, bool stopPlayback) {
    // Once the selected image has been updated, reflect that in the image info window.
    ScopeGuard imageInfoGuard{[this]() {
        if (mImageInfoWindow) {
            updateImageInfoWindow();
        }
    }};

    if (stopPlayback) {
        mPlayButton->set_pushed(false);
    }

    for (auto button : mCurrentImageButtons) {
        button->set_enabled(image != nullptr);
    }

    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(false);
        }

        mCurrentImage = nullptr;
        mImageCanvas->setImage(nullptr);

        // Clear group buttons
        while (mGroupButtonContainer->child_count() > 0) {
            mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
        }

        setImageWhiteLevel(DEFAULT_IMAGE_WHITE_LEVEL);

        requestLayoutUpdate();
        return;
    }

    const auto id = imageId(image).value_or(0);

    // Don't do anything if the image that wants to be selected is not visible.
    if (!mImageButtonContainer->child_at((int)id)->visible()) {
        return;
    }

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentImage = image;
    mImageCanvas->setImage(mCurrentImage);

    setImageWhiteLevel(mCurrentImage->whiteLevel());

    // Clear group buttons
    while (mGroupButtonContainer->child_count() > 0) {
        mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
    }

    const size_t numGroups = mCurrentImage->channelGroups().size();
    for (size_t i = 0; i < numGroups; ++i) {
        const auto group = groupName(i);
        const auto button = new ImageButton{mGroupButtonContainer, group, false};
        button->set_font_size(15);
        button->setId(i + 1);

        button->setSelectedCallback([this, group]() { selectGroup(group); });
    }

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Setting the filter again makes sure, that groups are correctly filtered.
    setFilter(mFilter->value());
    requestLayoutUpdate();

    // This will automatically fall back to the root group if the current group isn't found.
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
            mImageScrollContainer->set_scroll(clamp(
                mImageScrollContainer->scroll(),
                (activeImageButton->position().y() + activeImageButton->height() - mImageScrollContainer->height()) / divisor,
                activeImageButton->position().y() / divisor
            ));
        }
    }

    if (autoFitToScreen()) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

void ImageViewer::selectGroup(string_view group) {
    // If the group does not exist, select the first group.
    const auto id = groupId(group).value_or(0);

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
        mGroupButtonContainer->set_position(
            Vector2i{
                clamp(
                    mGroupButtonContainer->position().x(),
                    -activeGroupButton->position().x(),
                    m_size.x() - activeGroupButton->position().x() - activeGroupButton->width()
                ),
                0
            }
        );
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
            dynamic_cast<Button*>(metricButtons[i])->set_enabled(false);
        }

        mCurrentReference = nullptr;
        mImageCanvas->setReference(nullptr);
        return;
    }

    const auto id = imageId(image).value_or(0);

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsReference(i == id);
    }

    auto& metricButtons = mMetricButtonContainer->children();
    for (size_t i = 0; i < metricButtons.size(); ++i) {
        dynamic_cast<Button*>(metricButtons[i])->set_enabled(true);
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
            mImageScrollContainer->set_scroll(clamp(
                mImageScrollContainer->scroll(),
                (activeReferenceButton->position().y() + activeReferenceButton->height() - mImageScrollContainer->height()) / divisor,
                activeReferenceButton->position().y() / divisor
            ));
        }
    }
}

void ImageViewer::setExposure(float value) {
    value = round(value, 1.0f);
    mExposureSlider->set_value(value);
    mExposureLabel->set_caption(fmt::format("Exposure: {:+.1f}", value));

    mImageCanvas->setExposure(value);
}

void ImageViewer::setOffset(float value) {
    value = round(value, 2.0f);
    mOffsetSlider->set_value(value);
    mOffsetLabel->set_caption(fmt::format("Offset: {:+.2f}", value));

    mImageCanvas->setOffset(value);
}

void ImageViewer::setGamma(float value) {
    value = round(value, 2.0f);
    mGammaSlider->set_value(value);
    mGammaLabel->set_caption(fmt::format("Gamma: {:+.2f}", value));

    mImageCanvas->setGamma(value);
}

void ImageViewer::normalizeExposureAndOffset() {
    if (!mCurrentImage) {
        return;
    }

    const auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);

    float minimum = numeric_limits<float>::max();
    float maximum = numeric_limits<float>::min();
    for (const auto& channelName : channels) {
        const auto& channel = mCurrentImage->channel(channelName);
        auto [cmin, cmax, cmean] = channel->minMaxMean();
        maximum = std::max(maximum, cmax);
        minimum = std::min(minimum, cmin);
    }

    const float factor = 1.0f / (maximum - minimum);
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
        b->set_pushed((ETonemap)i == tonemap);
    }

    mGammaSlider->set_enabled(tonemap == ETonemap::Gamma);
    mGammaLabel->set_color(tonemap == ETonemap::Gamma ? mGammaLabel->theme()->m_text_color : Color{0.5f, 1.0f});
}

void ImageViewer::setMetric(EMetric metric) {
    mImageCanvas->setMetric(metric);
    auto& buttons = mMetricButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->set_pushed((EMetric)i == metric);
    }
}

void ImageViewer::setBackgroundColorStraight(const Color& color) {
    mBackgroundColorWheel->set_color(color);
    mBackgroundAlphaSlider->set_value(color.a());

    const Color premul = Color{color.r() * color.a(), color.g() * color.a(), color.b() * color.a(), color.a()};
    mImageCanvas->setBackgroundColor(premul);
}

float ImageViewer::displayWhiteLevel() const { return mDisplayWhiteLevelBox->value(); }

void ImageViewer::setDisplayWhiteLevel(float value) {
    mDisplayWhiteLevelBox->set_value(value);
    mImageCanvas->setWhiteLevelOverride(
        displayWhiteLevelSetting() != EDisplayWhiteLevelSetting::System && value > 0.0f ? optional<float>{displayWhiteLevel()} : nullopt
    );
}

void ImageViewer::setDisplayWhiteLevelToImageMetadata() {
    setDisplayWhiteLevel(mCurrentImage ? mCurrentImage->whiteLevel() : DEFAULT_IMAGE_WHITE_LEVEL);
}

void ImageViewer::setImageWhiteLevel(float value) {
    mImageWhiteLevelBox->set_value(value);
    if (displayWhiteLevelSetting() == EDisplayWhiteLevelSetting::ImageMetadata) {
        setDisplayWhiteLevelToImageMetadata();
    }
}

ImageViewer::EDisplayWhiteLevelSetting ImageViewer::displayWhiteLevelSetting() const {
    return static_cast<EDisplayWhiteLevelSetting>(mDisplayWhiteLevelSettingComboBox->selected_index());
}

void ImageViewer::setDisplayWhiteLevelSetting(EDisplayWhiteLevelSetting setting) {
    mDisplayWhiteLevelSettingComboBox->set_selected_index(static_cast<int>(setting));

    switch (displayWhiteLevelSetting()) {
        case EDisplayWhiteLevelSetting::System: setDisplayWhiteLevel(glfwGetWindowSdrWhiteLevel(m_glfw_window)); break;
        case EDisplayWhiteLevelSetting::Custom: break;
        case EDisplayWhiteLevelSetting::ImageMetadata: setDisplayWhiteLevelToImageMetadata();
    }
}

Vector2f ImageViewer::sizeToFitImage(const shared_ptr<Image>& image) {
    if (!image) {
        return m_size;
    }

    // Convert from image pixel coordinates to nanogui coordinates.
    auto requiredSize = Vector2f{image->displaySize()} / pixel_ratio();

    // Take into account the size of the UI.
    if (mSidebar->visible()) {
        requiredSize.x() += mSidebar->fixed_width();
    }

    if (mFooter->visible()) {
        requiredSize.y() += mFooter->fixed_height();
    }

    return requiredSize;
}

Vector2f ImageViewer::sizeToFitAllImages() {
    Vector2f result = m_size;
    for (const auto& image : mImages) {
        result = max(result, sizeToFitImage(image));
    }

    return result;
}

void ImageViewer::resizeToFit(Vector2f targetSize) {
    // On Wayland, some information like the current monitor or fractional DPI scaling is not available until some time has passed.
    // Potentially a few frames have been rendered. Hence postpone resizing until we have a valid monitor.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND && !glfwGetWindowCurrentMonitor(m_glfw_window)) {
        mDidFitToImage = 2;
        return;
    }

    // Only increase our current size if we are larger than the current size of the window.
    targetSize = max(Vector2f{m_size}, targetSize);
    // For sanity, don't make us larger than 8192x8192 to ensure that we don't break any texture size limitations of the user's GPU.

    auto maxSize = mMaxWindowSize;

    const Vector2f padding = {
#ifdef _WIN32
        2
#else
        0
#endif
    };

    maxSize -= 2 * padding;

    targetSize = min(targetSize, maxSize);
    if (targetSize == m_size) {
        return;
    }

    tlog::debug() << fmt::format("Resizing window to {}", targetSize);

    const auto sizeDiff = targetSize - Vector2f{m_size};

    set_size(targetSize);
    move_window(-sizeDiff / 2);

    // Ensure the window does not go off-screen by clamping its position. This does not work on Wayland, because Wayland does not allow
    // windows to control their own position. On Windows, we add additional padding because, otherwise, moving the mouse to the edge of the
    // screen does not allow the user to resize the window anymore.
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        const auto minWindowPos = Vector2i{mMinWindowPos + padding};
        const auto maxWindowPos = Vector2i{mMinWindowPos + maxSize - targetSize + padding};

        Vector2i pos;
        glfwGetWindowPos(m_glfw_window, &pos.x(), &pos.y());
        pos = min(max(pos, minWindowPos), maxWindowPos);
        glfwSetWindowPos(m_glfw_window, pos.x(), pos.y());
    }

    if (autoFitToScreen() && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::playingBack() const { return mPlayButton->pushed(); }

void ImageViewer::setPlayingBack(bool value) {
    mPlayButton->set_pushed(value);
    mLastPlaybackFrameTime = chrono::steady_clock::now();
    set_run_mode(value ? RunMode::VSync : RunMode::Lazy);
}

bool ImageViewer::setFilter(string_view filter) {
    mFilter->set_value(filter);
    mRequiresFilterUpdate = true;
    return true;
}

void ImageViewer::setFps(int value) { mFpsTextBox->set_value(value); }

bool ImageViewer::useRegex() const { return mRegexButton->pushed(); }

void ImageViewer::setUseRegex(bool value) {
    mRegexButton->set_pushed(value);
    mRequiresFilterUpdate = true;
}

bool ImageViewer::watchFilesForChanges() const { return mWatchFilesForChangesButton->pushed(); }

void ImageViewer::setWatchFilesForChanges(bool value) { mWatchFilesForChangesButton->set_pushed(value); }

bool ImageViewer::autoFitToScreen() const { return mAutoFitToScreenButton->pushed(); }

void ImageViewer::setAutoFitToScreen(bool value) {
    mAutoFitToScreenButton->set_pushed(value);
    if (value && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::resizeWindowToFitImageOnLoad() const { return mResizeWindowToFitImageOnLoadButton->pushed(); }

void ImageViewer::setResizeWindowToFitImageOnLoad(bool value) {
    mResizeWindowToFitImageOnLoadButton->set_pushed(value);
    if (value && mCurrentImage) {
        resizeToFit(sizeToFitImage(mCurrentImage));
    }
}

void ImageViewer::maximize() {
    glfwMaximizeWindow(m_glfw_window);
    if (autoFitToScreen() && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::isMaximized() { return !mMaximizedUnreliable && glfwGetWindowAttrib(m_glfw_window, GLFW_MAXIMIZED) != 0; }

void ImageViewer::toggleMaximized() {
    if (isMaximized()) {
        glfwRestoreWindow(m_glfw_window);
    } else {
        maximize();
    }
}

void ImageViewer::setUiVisible(bool shouldBeVisible) {
    if (!shouldBeVisible && mDragType == EMouseDragType::SidebarDrag) {
        mDragType = EMouseDragType::None;
    }

    mSidebar->set_visible(shouldBeVisible);
    mFooter->set_visible(mShouldFooterBeVisible && shouldBeVisible);

    requestLayoutUpdate();
}

void ImageViewer::toggleHelpWindow() {
    if (mHelpWindow) {
        mHelpWindow->dispose();
        mHelpWindow = nullptr;
        mHelpButton->set_pushed(false);
    } else {
        mHelpWindow = new HelpWindow{this, ipc(), [this] { toggleHelpWindow(); }};
        mHelpWindow->center();
        mHelpWindow->request_focus();
        mHelpButton->set_pushed(true);
    }

    requestLayoutUpdate();
}

void ImageViewer::toggleImageInfoWindow() {
    if (mImageInfoWindow) {
        mImageInfoWindow->dispose();
        mImageInfoWindow = nullptr;

        mImageInfoButton->set_pushed(false);
    } else {
        if (mCurrentImage) {
            mImageInfoWindow = new ImageInfoWindow{this, mCurrentImage, [this] { toggleImageInfoWindow(); }};
            mImageInfoWindow->center();
            mImageInfoWindow->request_focus();

            mImageInfoButton->set_pushed(true);
        }
    }

    requestLayoutUpdate();
}

void ImageViewer::updateImageInfoWindow() {
    if (mImageInfoWindow) {
        const auto pos = mImageInfoWindow->position();
        const auto size = mImageInfoWindow->size();
        const string tabName = string{mImageInfoWindow->currentTabName()};
        const float scroll = mImageInfoWindow->currentScroll();
        mImageInfoWindow->dispose();

        if (mCurrentImage) {
            mImageInfoWindow = new ImageInfoWindow{this, mCurrentImage, [this] { toggleImageInfoWindow(); }};
            mImageInfoWindow->set_position(pos);
            mImageInfoWindow->set_size(size);
            if (mImageInfoWindow->selectTabWithName(tabName)) {
                mImageInfoWindow->setScroll(scroll);
            }

            mImageInfoWindow->request_focus();

            mImageInfoButton->set_pushed(true);
        } else {
            mImageInfoWindow = nullptr;
            mImageInfoButton->set_pushed(false);
        }
    }
}

void ImageViewer::openImageDialog() {
    if (mFileDialogThread) {
        tlog::warning() << "File dialog already running.";
        return;
    }

    const auto runDialog = [this]() {
        const auto threadGuard = ScopeGuard{[this]() {
            scheduleToUiThread([this]() {
                focusWindow();
                if (mFileDialogThread && mFileDialogThread->joinable()) {
                    mFileDialogThread->join();
                }

                mFileDialogThread = nullptr;
            });
        }};

        try {
            vector<pair<string, string>> filters = {
                {"apng",                    "Animated PNG image"               },
#ifdef TEV_SUPPORT_AVIF
                {"avif",                    "AV1 Image File"                   },
#endif
                {"bmp",                     "Bitmap image"                     },
                {"cur",                     "Microsoft cursor image"           },
#ifdef _WIN32
                {"dds",                     "DirectDraw Surface image"         },
#endif
                {"dng",                     "Digital Negative image"           },
                {"exr",                     "OpenEXR image"                    },
                {"gif",                     "Graphics Interchange Format image"},
                {"hdr",                     "HDR image"                        },
#ifdef TEV_SUPPORT_HEIC
                {"heic",                    "High Efficiency Image Container"  },
#endif
                {"ico",                     "Microsoft icon image"             },
                {"jpeg,jpg",                "JPEG image"                       },
                {"jxl",                     "JPEG-XL image"                    },
                {"pam,pbm,pfm,pgm,pnm,ppm", "Portable *Map image"              },
                {"pic",                     "PIC image"                        },
                {"png",                     "Portable Network Graphics image"  },
                {"psd",                     "PSD image"                        },
                {"qoi",                     "Quite OK Image format"            },
                {"tga",                     "Truevision TGA image"             },
                {"tiff,tif",                "Tag Image File Format image"      },
                {"webp",                    "WebP image"                       },
            };

            vector<string_view> allImages;
            for (const auto& filter : filters) {
                allImages.push_back(filter.first);
            }

            filters.emplace(filters.begin(), pair<string, string>{join(allImages, ","), "All images"});
            const auto paths = file_dialog(this, FileDialogType::OpenMultiple, filters);

            for (size_t i = 0; i < paths.size(); ++i) {
                const bool shallSelect = i == paths.size() - 1;
                mImagesLoader->enqueue(paths[i], "", shallSelect);
            }
        } catch (const runtime_error& e) {
            const auto error = fmt::format("File dialog: {}", e.what());
            scheduleToUiThread([this, error]() { showErrorDialog(error); });
        }
    };

#if defined(__APPLE__) || defined(_WIN32)
    runDialog();
#else
    mFileDialogThread = make_unique<thread>(runDialog);
#endif
}

void ImageViewer::saveImageDialog() {
    if (!mCurrentImage) {
        return;
    }

    if (mFileDialogThread) {
        tlog::warning() << "File dialog already running.";
        return;
    }

    const auto runDialog = [this]() {
        const auto threadGuard = ScopeGuard{[this]() {
            scheduleToUiThread([this]() {
                focusWindow();
                if (mFileDialogThread && mFileDialogThread->joinable()) {
                    mFileDialogThread->join();
                }

                mFileDialogThread = nullptr;
            });
        }};

        try {
            const auto paths = file_dialog(
                this,
                FileDialogType::Save,
                {
                    {"exr",      "OpenEXR image"                  },
                    {"hdr",      "HDR image"                      },
                    {"bmp",      "Bitmap Image File"              },
                    {"jpg,jpeg", "JPEG image"                     },
                    {"jxl",      "JPEG-XL image"                  },
                    {"png",      "Portable Network Graphics image"},
                    {"qoi",      "Quite OK Image format"          },
                    {"tga",      "Truevision TGA image"           },
            }
            );

            if (paths.empty() || paths.front().empty()) {
                return;
            }

            scheduleToUiThread([this, path = paths.front()]() {
                try {
                    mImageCanvas->saveImage(path);
                } catch (const ImageSaveError& e) { showErrorDialog(fmt::format("Failed to save image: {}", e.what())); }
            });
        } catch (const runtime_error& e) {
            const auto error = fmt::format("Save dialog: {}", e.what());
            scheduleToUiThread([this, error]() { showErrorDialog(error); });
        }
    };

#if defined(__APPLE__) || defined(_WIN32)
    runDialog();
#else
    mFileDialogThread = make_unique<thread>(runDialog);
#endif
}

void ImageViewer::copyImageCanvasToClipboard() const {
    if (!mCurrentImage) {
        throw runtime_error{"No image selected for copy."};
    }

    const auto imageSize = mImageCanvas->imageDataSize();
    if (imageSize.x() == 0 || imageSize.y() == 0) {
        throw runtime_error{"Image canvas has no image data to copy to clipboard."};
    }

    const auto imageData = mImageCanvas->getRgbaLdrImageData(true, numeric_limits<int>::max()).get();

#if defined(__APPLE__) or defined(_WIN32)
    const clip::image image(
        imageData.data(),
        clip::image_spec{
            .width = (unsigned long)imageSize.x(),
            .height = (unsigned long)imageSize.y(),
            .bits_per_pixel = 32,
            .bytes_per_row = 4 * (unsigned long)imageSize.x(),
            .red_mask = 0x000000ff,
            .green_mask = 0x0000ff00,
            .blue_mask = 0x00ff0000,
            .alpha_mask = 0xff000000,
            .red_shift = 0,
            .green_shift = 8,
            .blue_shift = 16,
            .alpha_shift = 24
        }
    );

    if (!clip::set_image(image)) {
        throw runtime_error{"clip::set_image failed."};
    }
#else
    // TODO: make a dedicated PNG saver via libpng which should be faster than stb_image_write.
    const auto pngImageSaver = make_unique<StbiLdrImageSaver>();

    ostringstream pngData;
    try {
        pngImageSaver->save(pngData, "clipboard.png", imageData, imageSize, 4).get();
    } catch (const ImageSaveError& e) { throw runtime_error{fmt::format("Failed to save image data to clipboard as PNG: {}", e.what())}; }

    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        waylandSetClipboardPngImage(pngData.view());
    } else if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        clip::lock l;
        if (!l.locked()) {
            throw runtime_error{"Failed to lock clipboard."};
        }

        l.clear();
        if (!l.set_data(clip::image_format(), pngData.view().data(), pngData.view().size())) {
            throw runtime_error{"Failed to set image data to clipboard."};
        }
    }
#endif

    tlog::success() << "Image copied to clipboard.";
}

void ImageViewer::copyImageNameToClipboard() const {
    if (!mCurrentImage) {
        throw runtime_error{"No image selected for copy."};
    }

    glfwSetClipboardString(m_glfw_window, string{mCurrentImage->name()}.c_str());
    tlog::success() << "Image path copied to clipboard.";
}

void ImageViewer::pasteImagesFromClipboard() {
    stringstream imageStream;
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        const auto data = waylandGetClipboardPngImage();
        if (data.empty()) {
            throw runtime_error{"No image data found in clipboard."};
        }

        // TODO: use spanstream once it is available in C++23 to avoid the copy
        imageStream = stringstream{string{data}, ios::in};
    } else if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        clip::lock l;
        if (!l.locked()) {
            throw runtime_error{"Failed to lock clipboard."};
        }

        clip::format f = clip::image_format();
        if (!l.is_convertible(f)) {
            throw runtime_error{"Clipboard does not contain image data."};
        }

        const size_t len = l.get_data_length(f);
        string data(len, '\0');
        l.get_data(f, data.data(), len);

        imageStream = stringstream{std::move(data), ios::in};
    } else {
        clip::image clipImage;
        if (!clip::get_image(clipImage)) {
            throw runtime_error{"No image data found in clipboard."};
        }

        imageStream << "clip";
        imageStream.write(reinterpret_cast<const char*>(&clipImage.spec()), sizeof(clip::image_spec));
        imageStream.write(clipImage.data(), clipImage.spec().bytes_per_row * clipImage.spec().height);
    }

    tlog::info() << "Loading image from clipboard...";
    auto imagesLoadTask = tryLoadImage(
        fmt::format("clipboard ({})", ++mClipboardIndex), imageStream, "", mImagesLoader->imageLoaderSettings(), mImagesLoader->groupChannels()
    );

    const auto images = imagesLoadTask.get();

    if (images.empty()) {
        throw runtime_error{"Failed to load image from clipboard data."};
    } else {
        for (auto& image : images) {
            addImage(image, true);
        }
    }
}

void ImageViewer::showErrorDialog(string_view message) {
    tlog::error() << message;
    new MessageDialog(this, MessageDialog::Type::Warning, "Error", message);
}

chroma_t ImageViewer::inspectionChroma() const {
    TEV_ASSERT(mInspectionPrimariesBoxes.size() == 8, "Expected 8 color space primary boxes.");

    chroma_t chr;
    for (size_t i = 0; i < chr.size(); ++i) {
        for (size_t c = 0; c < 2; ++c) {
            chr[i][c] = mInspectionPrimariesBoxes.at(i * 2 + c)->value();
        }
    }

    return chr;
}

void ImageViewer::setInspectionChroma(const chroma_t& chr) {
    mImageCanvas->setInspectionChroma(chr);

    TEV_ASSERT(mInspectionPrimariesBoxes.size() == 8, "Expected 8 color space primary boxes.");

    for (size_t i = 0; i < chr.size(); ++i) {
        for (size_t c = 0; c < 2; ++c) {
            mInspectionPrimariesBoxes.at(i * 2 + c)->set_value(chr[i][c]);
        }
    }

    for (size_t i = 0; i < PRIMARIES.size(); ++i) {
        if (chr == chroma(PRIMARIES[i].first)) {
            mInspectionPrimariesComboBox->set_selected_index((int)i);
            return;
        }
    }

    // "Custom"
    mInspectionPrimariesComboBox->set_selected_index(PRIMARIES.size());
}

ituth273::ETransfer ImageViewer::inspectionTransfer() const {
    const size_t index = (size_t)mInspectionTransferComboBox->selected_index();
    TEV_ASSERT(index <= TRANSFERS.size(), "Invalid transfer function index selected for inspection.");

    return TRANSFERS.at(index).first;
}

void ImageViewer::setInspectionTransfer(const ituth273::ETransfer transfer) {
    mImageCanvas->setInspectionTransfer(transfer);

    for (size_t i = 0; i < TRANSFERS.size(); ++i) {
        if (transfer == TRANSFERS[i].first) {
            mInspectionTransferComboBox->set_selected_index((int)i);
            return;
        }
    }

    TEV_ASSERT(false, "Invalid transfer function specified for inspection.");
}

bool ImageViewer::inspectionAdaptWhitePoint() const { return mInspectionAdaptWhitePointButton->pushed(); }

void ImageViewer::setInspectionAdaptWhitePoint(bool value) {
    mImageCanvas->setInspectionAdaptWhitePoint(value);
    mInspectionAdaptWhitePointButton->set_pushed(value);
}

bool ImageViewer::inspectionPremultipliedAlpha() const { return mInspectionPremultipliedAlphaButton->pushed(); }

void ImageViewer::setInspectionPremultipliedAlpha(bool value) {
    mImageCanvas->setInspectionPremultipliedAlpha(value);
    mInspectionPremultipliedAlphaButton->set_pushed(value);
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
        // Checks whether an image matches the filter. This is the case if the image name matches the image part and at least one of the
        // image's groups matches the group part.
        auto doesImageMatch = [&](const auto& name, const auto& channelGroups) {
            bool doesMatch = matchesFuzzyOrRegex(name, imagePart, useRegex());
            if (doesMatch) {
                bool anyGroupsMatch = false;
                for (const auto& group : channelGroups) {
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
            ib->set_visible(doesImageMatch(ib->caption(), mImages[i]->channelGroups()));
            if (ib->visible()) {
                ib->setId(id++);
                activeImageNames.emplace_back(ib->caption());
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

                    allStartWithSameChar = all_of(begin(activeImageNames), end(activeImageNames), [&first, beginOffset, len](string_view name) {
                        if (beginOffset + len > (int)name.size()) {
                            return false;
                        }
                        for (int i = beginOffset; i < beginOffset + len; ++i) {
                            if (name[i] != first[i]) {
                                return false;
                            }
                        }
                        return true;
                    });

                    if (allStartWithSameChar) {
                        beginOffset += len;
                    }
                } while (allStartWithSameChar && beginOffset < firstSize);

                bool allEndWithSameChar;
                do {
                    char lastChar = first[firstSize - endOffset - 1];
                    allEndWithSameChar = all_of(begin(activeImageNames), end(activeImageNames), [lastChar, endOffset](string_view name) {
                        int index = (int)name.size() - endOffset - 1;
                        return index >= 0 && name[index] == lastChar;
                    });

                    if (allEndWithSameChar) {
                        ++endOffset;
                    }
                } while (allEndWithSameChar && endOffset < firstSize);
            }
        }

        bool currentImageMatchesFilter = false;
        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            if (ib->visible()) {
                currentImageMatchesFilter |= mImages[i] == mCurrentImage;
                ib->setHighlightRange(beginOffset, endOffset);
            }
        }

        if (!currentImageMatchesFilter) {
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
            ib->set_visible(matchesFuzzyOrRegex(ib->caption(), groupPart, useRegex()));
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
    mImageCanvas->set_fixed_size(m_size - Vector2i{sidebarWidth, footerHeight});
    mSidebar->set_fixed_height(m_size.y() - footerHeight);

    mVerticalScreenSplit->set_fixed_size(m_size);
    mImageScrollContainer->set_fixed_height(m_size.y() - mImageScrollContainer->position().y() - footerHeight);

    if (mImageScrollContainer->fixed_height() < 100) {
        // Stop scrolling the image button container and instead scroll the entire sidebar
        mImageScrollContainer->set_fixed_height(0);
    }

    mSidebarLayout->parent()->set_height(mSidebarLayout->preferred_size(m_nvg_context).y());
    perform_layout();

    mSidebarLayout->set_fixed_width(mSidebarLayout->parent()->width());
    mHelpButton->set_position(Vector2i{mSidebarLayout->fixed_width() - 38, 5});
    mFilter->set_fixed_width(mSidebarLayout->fixed_width() - 50);
    perform_layout();

    // With a changed layout the relative position of the mouse
    // within children changes and therefore should get updated.
    // nanogui does not handle this for us.
    double x, y;
    glfwGetCursorPos(m_glfw_window, &x, &y);
    cursor_pos_callback_event(x, y);
}

void ImageViewer::updateTitle() {
    if (!mCurrentImage) {
        set_caption("tev");
        return;
    }

    ostringstream caption;

    const auto channelsSpan = mCurrentImage->channelsInGroup(mCurrentGroup);
    vector<string_view> channels = {begin(channelsSpan), end(channelsSpan)};

    // Remove duplicates
    channels.erase(unique(begin(channels), end(channels)), end(channels));
    // Only treat alpha specially if it is not the only channel.
    const bool hasAlpha = channels.size() > 1 && Channel::isAlpha(channels.back());

    auto channelTails = channels;
    transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);

    caption << fmt::format("{} – {} – {}%", mCurrentImage->shortName(), mCurrentGroup, (int)std::round(mImageCanvas->scale() * 100));

    const auto rel = mouse_pos() - mImageCanvas->position();
    const vector<float> values = mImageCanvas->getValuesAtNanoPos({rel.x(), rel.y()}, channels);
    const Vector2i imageCoords = mImageCanvas->getImageCoords(mCurrentImage.get(), {rel.x(), rel.y()});
    TEV_ASSERT(values.size() >= channelTails.size(), "Should obtain a value for every existing channel.");

    caption << fmt::format(
        " – @{},{} ({:.3f},{:.3f}) / {}x{}: ",
        imageCoords.x(),
        imageCoords.y(),
        imageCoords.x() / (double)mCurrentImage->size().x(),
        imageCoords.y() / (double)mCurrentImage->size().y(),
        mCurrentImage->size().x(),
        mCurrentImage->size().y()
    );

    auto transformedValues = values;
    mImageCanvas->applyInspectionParameters(transformedValues, hasAlpha);
    for (size_t i = 0; i < transformedValues.size(); ++i) {
        caption << fmt::format("{:.2f},", transformedValues[i]);
    }

    caption.seekp(-1, ios_base::cur); // Remove last comma
    caption << " / 0x";
    for (size_t i = 0; i < values.size(); ++i) {
        const float srgbValue = hasAlpha && i == values.size() - 1 ? values[i] : toSRGB(values[i]);
        unsigned char discretizedValue = (char)(clamp(srgbValue, 0.0f, 1.0f) * 255 + 0.5f);
        caption << fmt::format("{:02X}", discretizedValue);
    }

    set_caption(caption.str());
}

string_view ImageViewer::groupName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    const auto groups = mCurrentImage->channelGroups();
    TEV_ASSERT(index < groups.size(), "Group index out of bounds.");
    return groups[index].name;
}

optional<size_t> ImageViewer::groupId(string_view groupName) const {
    if (!mCurrentImage) {
        return 0;
    }

    const auto groups = mCurrentImage->channelGroups();
    const auto pos = (size_t)distance(begin(groups), ranges::find(groups, groupName, [](const auto& g) -> string_view { return g.name; }));
    return pos >= groups.size() ? nullopt : optional{pos};
}

optional<size_t> ImageViewer::imageId(const shared_ptr<Image>& image) const {
    const auto pos = (size_t)distance(begin(mImages), ranges::find(mImages, image));
    return pos >= mImages.size() ? nullopt : optional{pos};
}

optional<size_t> ImageViewer::imageId(string_view imageName) const {
    const auto pos = (size_t)distance(begin(mImages), ranges::find(mImages, imageName, [](const auto& i) { return i->name(); }));
    return pos >= mImages.size() ? nullopt : optional{pos};
}

string_view ImageViewer::nextGroup(string_view group, EDirection direction) {
    if (mGroupButtonContainer->child_count() == 0) {
        return mCurrentGroup;
    }

    const auto dir = direction == Forward ? 1 : -1;

    // If the group does not exist, start at index 0.
    const auto startId = (int)groupId(group).value_or(0);

    auto id = startId;
    do {
        id = (id + mGroupButtonContainer->child_count() + dir) % mGroupButtonContainer->child_count();
    } while (!mGroupButtonContainer->child_at(id)->visible() && id != startId);

    return groupName(id);
}

string_view ImageViewer::nthVisibleGroup(size_t n) {
    auto visibleGroups = views::iota(0, mGroupButtonContainer->child_count()) |
        views::filter([&](int i) { return mGroupButtonContainer->child_at(i)->visible(); }) |
        views::transform([&](int i) { return groupName((size_t)i); }) | views::take(n + 1);

    string_view lastVisible = mCurrentGroup;
    for (auto group : visibleGroups) {
        lastVisible = group;
    }

    return lastVisible;
}

shared_ptr<Image> ImageViewer::nextImage(const shared_ptr<Image>& image, EDirection direction) {
    if (mImages.empty()) {
        return nullptr;
    }

    const auto dir = direction == Forward ? 1 : -1;

    // If the image does not exist, start at image 0.
    const auto startId = (int)imageId(image).value_or(0);

    auto id = startId;
    do {
        id = (id + mImageButtonContainer->child_count() + dir) % mImageButtonContainer->child_count();
    } while (!mImageButtonContainer->child_at(id)->visible() && id != startId);

    return mImages.at(id);
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

shared_ptr<Image> ImageViewer::imageByName(string_view imageName) {
    const auto id = imageId(imageName);
    return id ? mImages.at(*id) : nullptr;
}

void ImageViewer::updateCurrentMonitorSize() {
    if (GLFWmonitor* monitor = glfwGetWindowCurrentMonitor(m_glfw_window)) {
        Vector2i pos, size;
        glfwGetMonitorWorkarea(monitor, &pos.x(), &pos.y(), &size.x(), &size.y());
        if (size == Vector2i{0, 0}) {
            return;
        }

        // On some systems (notably Hyprland and some other tiling window managers / compositors), windows are always flagged as
        // maximized, even if they are technically not, to get them to play nicely with decorations. In the following, we detect
        // such cases (only after a current monitor was detected to give enough time for the compositor to set up the window) and
        // treat them as non-maximized always.
        if (isMaximized() && !mMaximizedLaunch) {
            tlog::debug() << "Detected unreliable maximized state; disabling maximized detection.";
            mMaximizedUnreliable = true;
        }

        auto posf = Vector2f{pos};
        auto sizef = Vector2f{size};

        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            posf = posf / pixel_ratio();
            sizef = sizef / pixel_ratio();
        }

        if (posf == mMinWindowPos && sizef == mMaxWindowSize) {
            return;
        }

        mMinWindowPos = posf;
        mMaxWindowSize = sizef;

        tlog::debug() << fmt::format("Current monitor: pos={} size={}", mMinWindowPos, mMaxWindowSize);
    }
}

} // namespace tev
