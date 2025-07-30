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

#include <tev/ImageViewer.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/ImageSaver.h>
#include <tev/imageio/StbiLdrImageSaver.h>

#include <clip.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/popupbutton.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>

#include <chrono>
#include <limits>
#include <stdexcept>

using namespace nanogui;
using namespace std;

namespace tev {

static const int SIDEBAR_MIN_WIDTH = 230;
static const float CROP_MIN_SIZE = 3;

ImageViewer::ImageViewer(
    const Vector2i& size, const shared_ptr<BackgroundImagesLoader>& imagesLoader, const shared_ptr<Ipc>& ipc, bool maximize, bool showUi, bool floatBuffer
) :
    nanogui::Screen{size, "tev", true, maximize, false, true, true, floatBuffer}, mImagesLoader{imagesLoader}, mIpc{ipc} {

    // If nanogui wants to apply color management, we need to actually provide a shader that converts color spaces.
    // nanogui only provides the necessary plumbing, defaulting to a passthrough shader that users need to actually implement.
#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    if (m_wants_color_management) {
        mDitherMatrix = new Texture{
            Texture::PixelFormat::R,
            Texture::ComponentFormat::Float32,
            Vector2i{nanogui::DITHER_MATRIX_SIZE},
            Texture::InterpolationMode::Nearest,
            Texture::InterpolationMode::Nearest,
            Texture::WrapMode::Repeat,
        };

        const float ditherScale = has_float_buffer() ? 0.0f : (1.0f / (1u << bits_per_sample()));
        auto ditherMatrix = nanogui::ditherMatrix(ditherScale);
        mDitherMatrix->upload((uint8_t*)ditherMatrix.data());

#    if defined(NANOGUI_USE_OPENGL)
        std::string preamble = "#version 110\n";
#    elif defined(NANOGUI_USE_GLES)
        std::string preamble = "#version 100\nprecision highp float; precision highp sampler2D;\n";
#    endif
        auto vertexShader = preamble + R"glsl(
            uniform vec2 ditherScale;

            attribute vec2 position;
            varying vec2 imageUv;
            varying vec2 ditherUv;

            void main() {
                vec2 pos = position * 0.5 + 0.5; // Convert from [-1, 1] to [0, 1]
                imageUv = pos;
                ditherUv = pos * ditherScale;

                gl_Position = vec4(position, 1.0, 1.0);
            }
        )glsl";
        auto fragmentShader = preamble + R"glsl(
            varying vec2 imageUv;
            varying vec2 ditherUv;

            uniform sampler2D framebufferTexture;
            uniform sampler2D ditherMatrix;

            uniform float displaySdrWhiteLevel;
            uniform float minLuminance;
            uniform float maxLuminance;

            uniform int outTransferFunction;
            uniform mat3 displayColorMatrix;
            uniform bool clipToUnitInterval;

            #define CM_TRANSFER_FUNCTION_BT1886     1
            #define CM_TRANSFER_FUNCTION_GAMMA22    2
            #define CM_TRANSFER_FUNCTION_GAMMA28    3
            #define CM_TRANSFER_FUNCTION_ST240      4
            #define CM_TRANSFER_FUNCTION_EXT_LINEAR 5
            #define CM_TRANSFER_FUNCTION_LOG_100    6
            #define CM_TRANSFER_FUNCTION_LOG_316    7
            #define CM_TRANSFER_FUNCTION_XVYCC      8
            #define CM_TRANSFER_FUNCTION_SRGB       9
            #define CM_TRANSFER_FUNCTION_EXT_SRGB   10
            #define CM_TRANSFER_FUNCTION_ST2084_PQ  11
            #define CM_TRANSFER_FUNCTION_ST428      12
            #define CM_TRANSFER_FUNCTION_HLG        13

            #define SRGB_POW 2.4
            #define SRGB_CUT 0.0031308
            #define SRGB_SCALE 12.92
            #define SRGB_ALPHA 1.055

            #define BT1886_POW (1.0 / 0.45)
            #define BT1886_CUT 0.018053968510807
            #define BT1886_SCALE 4.5
            #define BT1886_ALPHA (1.0 + 5.5 * BT1886_CUT)

            // See http://car.france3.mars.free.fr/HD/INA-%2026%20jan%2006/SMPTE%20normes%20et%20confs/s240m.pdf
            #define ST240_POW (1.0 / 0.45)
            #define ST240_CUT 0.0228
            #define ST240_SCALE 4.0
            #define ST240_ALPHA 1.1115

            #define ST428_POW 2.6
            #define ST428_SCALE (52.37 / 48.0)

            #define PQ_M1 0.1593017578125
            #define PQ_M2 78.84375
            #define PQ_INV_M1 (1.0 / PQ_M1)
            #define PQ_INV_M2 (1.0 / PQ_M2)
            #define PQ_C1 0.8359375
            #define PQ_C2 18.8515625
            #define PQ_C3 18.6875

            #define HLG_D_CUT (1.0 / 12.0)
            #define HLG_E_CUT 0.5
            #define HLG_A 0.17883277
            #define HLG_B 0.28466892
            #define HLG_C 0.55991073

            #define M_E 2.718281828459045

            vec3 mixb(vec3 a, vec3 b, bvec3 mask) {
                return mix(a, b, vec3(mask));
            }

            // The primary source for these transfer functions is https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1361-0-199802-W!!PDF-E.pdf
            // Outputs are assumed to have 1 == SDR White which is different for each transfer function.
            vec3 tfInvPQ(vec3 color) {
                vec3 E = pow(max(color.rgb, vec3(0.0)), vec3(PQ_INV_M2));
                return pow(
                    (max(E - PQ_C1, vec3(0.0))) / max(PQ_C2 - PQ_C3 * E, vec3(1e-5)),
                    vec3(PQ_INV_M1)
                );
            }

            vec3 tfInvHLG(vec3 color) {
                bvec3 isLow = lessThanEqual(color.rgb, vec3(HLG_E_CUT));
                vec3 lo = color.rgb * color.rgb / 3.0;
                vec3 hi = (exp((color.rgb - HLG_C) / HLG_A) + HLG_B) / 12.0;
                return mixb(hi, lo, isLow);
            }

            // Many transfer functions (including sRGB) follow the same pattern: a linear
            // segment for small values and a power function for larger values. The
            // following function implements this pattern from which sRGB, BT.1886, and
            // others can be derived by plugging in the right constants.
            vec3 tfInvLinPow(vec3 color, float gamma, float thres, float scale, float alpha) {
                bvec3 isLow = lessThanEqual(color.rgb, vec3(thres * scale));
                vec3 lo = color.rgb / scale;
                vec3 hi = pow((color.rgb + alpha - 1.0) / alpha, vec3(gamma));
                return mixb(hi, lo, isLow);
            }

            vec3 tfInvSRGB(vec3 color) {
                return tfInvLinPow(color, SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
            }

            vec3 tfInvExtSRGB(vec3 color) {
                // EXT sRGB is the sRGB transfer function mirrored around 0.
                return sign(color) * tfInvSRGB(abs(color));
            }

            vec3 tfInvBT1886(vec3 color) {
                return tfInvLinPow(color, BT1886_POW, BT1886_CUT, BT1886_SCALE, BT1886_ALPHA);
            }

            vec3 tfInvXVYCC(vec3 color) {
                // The inverse transfer function for XVYCC is the BT1886 transfer function mirrored around 0,
                // same as what EXT sRGB is to sRGB.
                return sign(color) * tfInvBT1886(abs(color));
            }

            vec3 tfInvST240(vec3 color) {
                return tfInvLinPow(color, ST240_POW, ST240_CUT, ST240_SCALE, ST240_ALPHA);
            }

            // Forward transfer functions corresponding to the inverse functions above.
            // Inputs are assumed to have 1 == 80 nits with a scale factor pre-applied to adjust for SDR white!
            vec3 tfPQ(vec3 color) {
                vec3 E = pow(max(color.rgb, vec3(0.0)), vec3(PQ_M1));
                return pow(
                    (vec3(PQ_C1) + PQ_C2 * E) / max(vec3(1.0) + PQ_C3 * E, vec3(1e-5)),
                    vec3(PQ_M2)
                );
            }

            vec3 tfHLG(vec3 color) {
                bvec3 isLow = lessThanEqual(color.rgb, vec3(HLG_D_CUT));
                vec3 lo = sqrt(max(color.rgb, vec3(0.0)) * 3.0);
                vec3 hi = HLG_A * log(max(12.0 * color.rgb - HLG_B, vec3(0.0001))) + HLG_C;
                return mixb(hi, lo, isLow);
            }

            vec3 tfLinPow(vec3 color, float gamma, float thres, float scale, float alpha) {
                bvec3 isLow = lessThanEqual(color.rgb, vec3(thres));
                vec3 lo = color.rgb * scale;
                vec3 hi = pow(color.rgb, vec3(1.0 / gamma)) * alpha - (alpha - 1.0);
                return mixb(hi, lo, isLow);
            }

            vec3 tfSRGB(vec3 color) {
                return tfLinPow(color, SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
            }

            vec3 tfExtSRGB(vec3 color) {
                // EXT sRGB is the sRGB transfer function mirrored around 0.
                return sign(color) * tfSRGB(abs(color));
            }

            vec3 tfBT1886(vec3 color) {
                return tfLinPow(color, BT1886_POW, BT1886_CUT, BT1886_SCALE, BT1886_ALPHA);
            }

            vec3 tfXVYCC(vec3 color) {
                // The transfer function for XVYCC is the BT1886 transfer function mirrored around 0,
                // same as what EXT sRGB is to sRGB.
                return sign(color) * tfBT1886(abs(color));
            }

            vec3 tfST240(vec3 color) {
                return tfLinPow(color, ST240_POW, ST240_CUT, ST240_SCALE, ST240_ALPHA);
            }

            vec3 toLinearRGB(vec3 color, int tf) {
                if (tf == CM_TRANSFER_FUNCTION_EXT_LINEAR) {
                    return color;
                } else if (tf == CM_TRANSFER_FUNCTION_ST2084_PQ) {
                    return tfInvPQ(color);
                } else if (tf == CM_TRANSFER_FUNCTION_GAMMA22) {
                    return pow(max(color, vec3(0.0)), vec3(2.2));
                } else if (tf == CM_TRANSFER_FUNCTION_GAMMA28) {
                    return pow(max(color, vec3(0.0)), vec3(2.8));
                } else if (tf == CM_TRANSFER_FUNCTION_HLG) {
                    return tfInvHLG(color);
                } else if (tf == CM_TRANSFER_FUNCTION_EXT_SRGB) {
                    return tfInvExtSRGB(color);
                } else if (tf == CM_TRANSFER_FUNCTION_BT1886) {
                    return tfInvBT1886(color);
                } else if (tf == CM_TRANSFER_FUNCTION_ST240) {
                    return tfInvST240(color);
                } else if (tf == CM_TRANSFER_FUNCTION_LOG_100) {
                    return mixb(exp((color - 1.0) * 2.0 * log(10.0)), vec3(0.0), lessThanEqual(color, vec3(0.0)));
                } else if (tf == CM_TRANSFER_FUNCTION_LOG_316) {
                    return mixb(exp((color - 1.0) * 2.5 * log(10.0)), vec3(0.0), lessThanEqual(color, vec3(0.0)));
                } else if (tf == CM_TRANSFER_FUNCTION_XVYCC) {
                    return tfInvXVYCC(color);
                } else if (tf == CM_TRANSFER_FUNCTION_ST428) {
                    return pow(max(color, vec3(0.0)), vec3(ST428_POW)) * ST428_SCALE;
                } else if (tf == CM_TRANSFER_FUNCTION_SRGB) {
                    return tfInvSRGB(color);
                } else {
                    return tfInvSRGB(color);
                }
            }

            vec3 fromLinearRGB(vec3 color, int tf) {
                if (tf == CM_TRANSFER_FUNCTION_EXT_LINEAR) {
                    return color;
                } else if (tf == CM_TRANSFER_FUNCTION_ST2084_PQ) {
                    return tfPQ(color);
                } else if (tf == CM_TRANSFER_FUNCTION_GAMMA22) {
                    return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
                } else if (tf == CM_TRANSFER_FUNCTION_GAMMA28) {
                    return pow(max(color, vec3(0.0)), vec3(1.0 / 2.8));
                } else if (tf == CM_TRANSFER_FUNCTION_HLG) {
                    return tfHLG(color);
                } else if (tf == CM_TRANSFER_FUNCTION_EXT_SRGB) {
                    return tfExtSRGB(color);
                } else if (tf == CM_TRANSFER_FUNCTION_BT1886) {
                    return tfBT1886(color);
                } else if (tf == CM_TRANSFER_FUNCTION_ST240) {
                    return tfST240(color);
                } else if (tf == CM_TRANSFER_FUNCTION_LOG_100) {
                    return mixb(1.0 + log(color) / log(10.0) / 2.0, vec3(0.0), lessThanEqual(color, vec3(0.01)));
                } else if (tf == CM_TRANSFER_FUNCTION_LOG_316) {
                    return mixb(1.0 + log(color) / log(10.0) / 2.5, vec3(0.0), lessThanEqual(color, vec3(sqrt(10.0) / 1000.0)));
                } else if (tf == CM_TRANSFER_FUNCTION_XVYCC) {
                    return tfXVYCC(color);
                } else if (tf == CM_TRANSFER_FUNCTION_ST428) {
                    return pow(max(color, vec3(0.0)) / ST428_SCALE, vec3(1.0 / ST428_POW));
                } else if (tf == CM_TRANSFER_FUNCTION_SRGB) {
                    return tfSRGB(color);
                } else {
                    return tfSRGB(color);
                }
            }

            float transferWhiteLevel(int tf) {
                if (tf == CM_TRANSFER_FUNCTION_ST2084_PQ) {
                    return 10000.0;
                } else if (tf == CM_TRANSFER_FUNCTION_HLG) {
                    return 1000.0;
                } else if (tf == CM_TRANSFER_FUNCTION_BT1886) {
                    return 100.0;
                } else if (tf == CM_TRANSFER_FUNCTION_XVYCC) {
                    return 100.0;
                } else {
                    return 80.0;
                }
            }

            vec3 dither(vec3 color) {
                return color + texture2D(ditherMatrix, fract(ditherUv)).r;
            }

            void main() {
                vec4 color = texture2D(framebufferTexture, imageUv);

                // tev handles colors in linear sRGB with a scale that assumes SDR white corresponds to a value of 1. Hence, to convert to
                // absolute nits in the display's color space, we need to multiply by the SDR white level of the display, as well as its
                // color transform.
                vec3 nits = displayColorMatrix * (displaySdrWhiteLevel * toLinearRGB(color.rgb, CM_TRANSFER_FUNCTION_EXT_SRGB));

                // Some displays perform strange tonemapping when provided with values outside of their luminance range. Make sure we don't
                // let this happen -- we strongly prefer hard clipping because we want the displayable colors to be preserved.
                if (maxLuminance > 0.0) {
                    nits = clamp(nits, vec3(minLuminance), vec3(maxLuminance));
                }

                color.rgb = dither(fromLinearRGB(nits / transferWhiteLevel(outTransferFunction), outTransferFunction));
                if (clipToUnitInterval) {
                    color = clamp(color, vec4(0.0), vec4(1.0));
                }

                gl_FragColor = color;
            }
        )glsl";

        try {
            m_cm_shader = new Shader(nullptr, "color_management", vertexShader, fragmentShader);
        } catch (const std::runtime_error& e) { fprintf(stderr, "Error creating color management shader: %s\n", e.what()); }

        uint32_t indices[3 * 2] = {0, 1, 2, 2, 3, 0};
        float positions[2 * 4] = {-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f};

        m_cm_shader->set_buffer("indices", VariableType::UInt32, {3 * 2}, indices);
        m_cm_shader->set_buffer("position", VariableType::Float32, {4, 2}, positions);
        m_cm_shader->set_texture("ditherMatrix", mDitherMatrix);
    }
#endif

    auto tf = ituth273::fromWpTransfer(glfwGetWindowTransfer(m_glfw_window));
    mSupportsHdr = m_float_buffer || tf == ituth273::ETransferCharacteristics::PQ || tf == ituth273::ETransferCharacteristics::HLG;

    tlog::info() << fmt::format(
        "Obtained {} bit {} point frame buffer with primaries={} and transfer={}.{}",
        this->bits_per_sample(),
        m_float_buffer ? "float" : "fixed",
        wpPrimariesToString(glfwGetWindowPrimaries(m_glfw_window)),
        ituth273::toString(tf),
        mSupportsHdr ? " HDR display is supported." : " HDR is *not* supported."
    );

    // At this point we no longer need the standalone console (if it exists).
    toggleConsole();

    // Get monitor configuration to figure out how large the tev window may maximally become.
    {
        int monitorCount;
        auto** monitors = glfwGetMonitors(&monitorCount);
        if (monitors && monitorCount > 0) {
            nanogui::Vector2i monitorMin{numeric_limits<int>::max(), numeric_limits<int>::max()},
                monitorMax{numeric_limits<int>::min(), numeric_limits<int>::min()};

            for (int i = 0; i < monitorCount; ++i) {
                nanogui::Vector2i pos, size;
                glfwGetMonitorWorkarea(monitors[i], &pos.x(), &pos.y(), &size.x(), &size.y());
                monitorMin = min(monitorMin, pos);
                monitorMax = max(monitorMax, pos + size);
            }

            mMaxSize = min(mMaxSize, max(monitorMax - monitorMin, nanogui::Vector2i{1024, 800}));
        }
    }

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
        buttonContainer->set_layout(new GridLayout{Orientation::Horizontal, mSupportsHdr ? 4 : 3, Alignment::Fill, 5, 2});

        auto makeButton = [&](string_view name, function<void()> callback, int icon = 0, string_view tooltip = "") {
            auto button = new Button{buttonContainer, string{name}, icon};
            button->set_font_size(15);
            button->set_callback(callback);
            button->set_tooltip(string{tooltip});
            return button;
        };

        mCurrentImageButtons.push_back(makeButton("Normalize", [this]() { normalizeExposureAndOffset(); }, 0, "Shortcut: N"));
        makeButton("Reset", [this]() { resetImage(); }, 0, "Shortcut: R");

        if (mSupportsHdr) {
            mClipToLdrButton = new Button{buttonContainer, "LDR", 0};
            mClipToLdrButton->set_font_size(15);
            mClipToLdrButton->set_change_callback([this](bool value) { mImageCanvas->setClipToLdr(value); });
            mClipToLdrButton->set_tooltip(
                "Clips the image to [0,1] as if displayed on an LDR screen.\n\n"
                "Shortcut: L"
            );
            mClipToLdrButton->set_flags(Button::ToggleButton);
        }

        auto popupBtn = new PopupButton{buttonContainer, "", FA_PAINT_BRUSH};
        popupBtn->set_font_size(15);
        popupBtn->set_chevron_icon(0);
        popupBtn->set_tooltip("Background Color");

        // Background color popup
        {
            auto popup = popupBtn->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            new Label{popup, "Background Color"};
            auto colorwheel = new ColorWheel{popup, mImageCanvas->backgroundColor()};
            colorwheel->set_color(popupBtn->background_color());

            new Label{popup, "Background Alpha"};
            auto bgAlphaSlider = new Slider{popup};
            bgAlphaSlider->set_range({0.0f, 1.0f});
            bgAlphaSlider->set_callback([this](float value) {
                auto col = mImageCanvas->backgroundColor();
                mImageCanvas->setBackgroundColor(
                    Color{
                        col.r(),
                        col.g(),
                        col.b(),
                        value,
                    }
                );
            });

            bgAlphaSlider->set_value(0);

            colorwheel->set_callback([bgAlphaSlider, this](const Color& value) {
                // popupBtn->set_background_color(value);
                mImageCanvas->setBackgroundColor(
                    Color{
                        value.r(),
                        value.g(),
                        value.b(),
                        bgAlphaSlider->value(),
                    }
                );
            });
        }
    }

    // Tonemap options
    {
        mTonemapButtonContainer = new Widget{mSidebarLayout};
        mTonemapButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

        auto makeTonemapButton = [&](string_view name, function<void()> callback) {
            auto button = new Button{mTonemapButtonContainer, string{name}};
            button->set_flags(Button::RadioButton);
            button->set_font_size(15);
            button->set_callback(callback);
            return button;
        };

        makeTonemapButton("sRGB", [this]() { setTonemap(ETonemap::SRGB); });
        makeTonemapButton("Gamma", [this]() { setTonemap(ETonemap::Gamma); });
        makeTonemapButton("FC", [this]() { setTonemap(ETonemap::FalseColor); });
        makeTonemapButton("+/-", [this]() { setTonemap(ETonemap::PositiveNegative); });

        setTonemap(ETonemap::SRGB);

        mTonemapButtonContainer->set_tooltip(
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
        mMetricButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

        auto makeMetricButton = [&](string_view name, function<void()> callback) {
            auto button = new Button{mMetricButtonContainer, string{name}};
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
                    "Keyboard shortcut:\n{}+P",
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
            playback->set_layout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

            auto makePlaybackButton = [&](string_view name, bool enabled, function<void()> callback, int icon = 0, string_view tooltip = "") {
                auto button = new Button{playback, string{name}, icon};
                button->set_callback(callback);
                button->set_tooltip(string{tooltip});
                button->set_font_size(15);
                button->set_enabled(enabled);
                button->set_padding({10, 10});
                return button;
            };

            mPlayButton = makePlaybackButton("", true, [] {}, FA_PLAY, "Play (Space)");
            mPlayButton->set_flags(Button::ToggleButton);
            mPlayButton->set_change_callback([this](bool value) { setPlayingBack(value); });

            mAnyImageButtons.push_back(makePlaybackButton(
                "", false, [this] { selectImage(nthVisibleImage(0)); }, FA_FAST_BACKWARD, "Front (Home)"
            ));

            mAnyImageButtons.push_back(makePlaybackButton(
                "", false, [this] { selectImage(nthVisibleImage(mImages.size())); }, FA_FAST_FORWARD, "Back (End)"
            ));

            mFpsTextBox = new IntBox<int>{playback, 24};
            mFpsTextBox->set_default_value("24");
            mFpsTextBox->set_units("fps");
            mFpsTextBox->set_editable(true);
            mFpsTextBox->set_alignment(TextBox::Alignment::Right);
            mFpsTextBox->set_min_max_values(1, 1000);
            mFpsTextBox->set_spinnable(true);
            mFpsTextBox->set_size(30);

            mAutoFitToScreenButton =
                makePlaybackButton("", true, {}, FA_EXPAND_ARROWS_ALT, "Automatically fit image to screen upon selection.");
            mAutoFitToScreenButton->set_flags(Button::Flags::ToggleButton);
            mAutoFitToScreenButton->set_change_callback([this](bool value) { setAutoFitToScreen(value); });
        }

        // Save, refresh, load, close
        {
            auto tools = new Widget{mSidebarLayout};
            tools->set_layout(new GridLayout{Orientation::Horizontal, 7, Alignment::Fill, 5, 1});

            auto makeImageButton = [&](string_view name, bool enabled, function<void()> callback, int icon = 0, string_view tooltip = "") {
                auto button = new Button{tools, string{name}, icon};
                button->set_callback(callback);
                button->set_tooltip(string{tooltip});
                button->set_font_size(15);
                button->set_enabled(enabled);
                button->set_padding({10, 10});
                return button;
            };

            makeImageButton("", true, [this] { openImageDialog(); }, FA_FOLDER, fmt::format("Open ({}+O)", HelpWindow::COMMAND));

            mCurrentImageButtons.push_back(makeImageButton(
                "", false, [this] { saveImageDialog(); }, FA_SAVE, fmt::format("Save ({}+S)", HelpWindow::COMMAND)
            ));

            mCurrentImageButtons.push_back(makeImageButton(
                "", false, [this] { reloadImage(mCurrentImage); }, FA_RECYCLE, fmt::format("Reload ({}+R or F5)", HelpWindow::COMMAND)
            ));

            mAnyImageButtons.push_back(makeImageButton(
                "A", false, [this] { reloadAllImages(); }, 0, fmt::format("Reload All ({}+Shift+R or {}+F5)", HelpWindow::COMMAND, HelpWindow::COMMAND)
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
                fmt::format("Close ({}+W); Close All ({}+Shift+W)", HelpWindow::COMMAND, HelpWindow::COMMAND)
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

    set_resize_callback([this](nanogui::Vector2i) { requestLayoutUpdate(); });

    selectImage(nullptr);
    selectReference(nullptr);

    if (!maximize) {
        // mDidFitToImage is only used when starting out maximized and wanting to fit the window to the image size after *unmaximizing*.
        mDidFitToImage = 3;
    }

    updateLayout();

    mInitialized = true;
}

bool ImageViewer::resize_event(const Vector2i& size) {
    mImageCanvas->setPixelRatio(pixel_ratio());
    requestLayoutUpdate();

    return Screen::resize_event(size);
}

bool ImageViewer::mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) {
    redraw();

    // Check if the user performed mousedown on an imagebutton so we can mark it as being dragged. This has to occur before
    // Screen::mouse_button_event as the button would absorb the event.
    if (down) {
        if (mImageScrollContainer->contains(p - mSidebarLayout->parent()->position())) {
            auto& buttons = mImageButtonContainer->children();

            nanogui::Vector2i relMousePos = (absolute_position() + p) - mImageButtonContainer->absolute_position();

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
                mImageCanvas->setCrop(std::nullopt);
            }
        }

        mDragType = EMouseDragType::None;
    }

    return false;
}

bool ImageViewer::mouse_motion_event(const nanogui::Vector2i& p, const nanogui::Vector2i& rel, int button, int modifiers) {
    if (Screen::mouse_motion_event(p, rel, button, modifiers)) {
        return true;
    }

    // Only need high refresh rate responsiveness if tev is actually in focus.
    if (focused()) {
        redraw();
    }

    bool shouldShowResizeCursor = mDragType == EMouseDragType::SidebarDrag || canDragSidebarFrom(p);
    Cursor cursorType = shouldShowResizeCursor ? Cursor::HResize : Cursor::Arrow;

    mSidebarLayout->set_cursor(cursorType);
    mImageCanvas->set_cursor(cursorType);

    switch (mDragType) {
        case EMouseDragType::SidebarDrag:
            mSidebar->set_fixed_width(clamp(p.x(), SIDEBAR_MIN_WIDTH, m_size.x() - 10));
            requestLayoutUpdate();
            break;

        case EMouseDragType::ImageDrag: {
            nanogui::Vector2f relativeMovement = {rel};
            auto* glfwWindow = screen()->glfw_window();
            // There is no explicit access to the currently pressed modifier keys here, so we need to directly ask GLFW.
            if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                relativeMovement /= 10;
            } else if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL)) {
                relativeMovement /= std::log2(1.1f);
            }

            // If left mouse button is held, move the image with mouse movement
            if ((button & 1) != 0) {
                mImageCanvas->translate(relativeMovement);
            }

            // If middle mouse button is held, zoom in-out with up-down mouse movement
            if ((button & 4) != 0) {
                mImageCanvas->scale(relativeMovement.y() / 10.0f, Vector2f{mDraggingStartPosition});
            }

            break;
        }

        case EMouseDragType::ImageCrop: {
            Vector2i relStartMousePos = (absolute_position() + mDraggingStartPosition) - mImageCanvas->absolute_position();
            Vector2i relMousePos = (absolute_position() + p) - mImageCanvas->absolute_position();

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
            nanogui::Vector2i relMousePos = (absolute_position() + p) - mImageButtonContainer->absolute_position();

            TEV_ASSERT(mDraggedImageButtonId < buttons.size(), "Dragged image button id is out of bounds.");
            auto* draggedImgButton = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId]);
            for (size_t i = 0; i < buttons.size(); ++i) {
                if (i == mDraggedImageButtonId) {
                    continue;
                }

                auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos)) {
                    nanogui::Vector2i pos = imgButton->position();
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

    return false;
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
    redraw();
    return true;
}

bool ImageViewer::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    redraw();

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
            if (mCurrentImage) {
                int id = imageId(mCurrentImage);
                dynamic_cast<ImageButton*>(mImageButtonContainer->child_at(id))->showTextBox();
                requestLayoutUpdate();
            }
            return true;
        } else if (key == GLFW_KEY_N) {
            normalizeExposureAndOffset();
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
            try {
                pasteImagesFromClipboard();
            } catch (const runtime_error& e) { showErrorDialog(fmt::format("Failed to paste image from clipboard: {}", e.what())); }
            return true;
        }
    }

    // Keybindings which should respond to repeats
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_KP_ADD || key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
            float scaleAmount = 1.0f;
            if (modifiers & GLFW_MOD_SHIFT) {
                scaleAmount /= 10;
            } else if (modifiers & GLFW_MOD_CONTROL) {
                scaleAmount /= std::log2(1.1f);
            }

            if (key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
                scaleAmount = -scaleAmount;
            }

            nanogui::Vector2f origin = nanogui::Vector2f{mImageCanvas->position()} + nanogui::Vector2f{mImageCanvas->size()} * 0.5f;

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
                setTonemap(static_cast<ETonemap>((tonemap() + 1) % NumTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() + 1) % NumMetrics));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Forward));
            }

            return true;
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A || key == GLFW_KEY_LEFT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() - 1 + NumTonemaps) % NumTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() - 1 + NumMetrics) % NumMetrics));
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

    return false;
}

void ImageViewer::focusWindow() { glfwFocusWindow(m_glfw_window); }

void ImageViewer::draw_contents() {
    if (!mInitialized) {
        return;
    }

    // Color management
    if (m_cm_shader) {
        float displaySdrWhiteLevel = m_display_sdr_white_level_override ? m_display_sdr_white_level_override.value() :
                                                                          glfwGetWindowSdrWhiteLevel(m_glfw_window);
        m_cm_shader->set_uniform("displaySdrWhiteLevel", displaySdrWhiteLevel);
        m_cm_shader->set_uniform("outTransferFunction", (int)glfwGetWindowTransfer(m_glfw_window));

        const auto displayChroma = chromaFromWpPrimaries(glfwGetWindowPrimaries(m_glfw_window));
        const auto displayColorMatrix = inverse(toMatrix3(chromaToRec709Matrix(displayChroma)));
        m_cm_shader->set_uniform("displayColorMatrix", displayColorMatrix);

        m_cm_shader->set_uniform("minLuminance", glfwGetWindowMinLuminance(m_glfw_window));
        m_cm_shader->set_uniform("maxLuminance", glfwGetWindowMaxLuminance(m_glfw_window));

        m_cm_shader->set_uniform("clipToUnitInterval", !m_float_buffer);

        m_cm_shader->set_uniform("ditherScale", (1.0f / DITHER_MATRIX_SIZE) * Vector2f(m_fbsize[0], m_fbsize[1]));
        m_cm_shader->set_texture("ditherMatrix", mDitherMatrix);
    }

    // HACK HACK HACK: on Windows, when restoring a window from maximization, the old window size is restored _several times_, necessitating
    // a repeated resize to the actually desired window size.
    if (mDidFitToImage < 3 && !isMaximized()) {
        resizeToFit(sizeToFitAllImages());
        ++mDidFitToImage;
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

        redraw();
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
        if (!isShown || std::chrono::steady_clock::now() - mImageCanvas->canvasStatistics()->becameReadyAt() > 200ms) {
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
        std::any_of(begin(mImageButtonContainer->children()), end(mImageButtonContainer->children()), [](const auto& c) {
                               return c->visible();
                           });

    for (auto button : mAnyImageButtons) {
        button->set_enabled(anyImageVisible);
    }

    if (mRequiresLayoutUpdate) {
        nanogui::Vector2i oldDraggedImageButtonPos{0, 0};
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
    static const string histogramTooltipBase = "Histogram of color values. Adapts to the currently chosen channel group and error metric.";
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
        mHistogram->setColors({{1.0f, 1.0f, 1.0f}});
        mHistogram->setValues({{0.0f}});
        mHistogram->setMinimum(0);
        mHistogram->setMean(0);
        mHistogram->setMaximum(0);
        mHistogram->setZero(0);
        mHistogram->set_tooltip(fmt::format("{}", histogramTooltipBase));
    }
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
        if (!isMaximized()) {
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
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    if (mDragType == EMouseDragType::ImageButtonDrag) {
        // If we're currently dragging the to-be-removed image, stop.
        if ((size_t)id == mDraggedImageButtonId) {
            requestLayoutUpdate();
            mDragType = EMouseDragType::None;
        } else if ((size_t)id < mDraggedImageButtonId) {
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

    mImages.erase(begin(mImages) + id);
    mImageButtonContainer->remove_child_at(id);

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
        throw std::runtime_error{"Must not replace image with nullptr."};
    }

    const int currentId = imageId(mCurrentImage);
    const int id = imageId(image);
    if (id == -1) {
        addImage(replacement, shallSelect);
        return;
    }

    // Preserve image button caption when replacing an image
    ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[id]);
    const std::string caption{ib->caption()};

    // If we already have the image selected, we must re-select it regardless of the `shallSelect` parameter.
    shallSelect |= currentId == id;

    int referenceId = imageId(mCurrentReference);

    removeImage(image);
    insertImage(replacement, id, shallSelect);

    ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[id]);
    ib->setCaption(caption);

    if (referenceId != -1) {
        selectReference(mImages[referenceId]);
    }
}

void ImageViewer::reloadImage(shared_ptr<Image> image, bool shallSelect) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    mImagesLoader->enqueue(image->path(), image->channelSelector(), shallSelect, image);
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

        requestLayoutUpdate();
        return;
    }

    size_t id = (size_t)max(0, imageId(image));

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

    // Clear group buttons
    while (mGroupButtonContainer->child_count() > 0) {
        mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
    }

    size_t numGroups = mCurrentImage->channelGroups().size();
    for (size_t i = 0; i < numGroups; ++i) {
        auto group = groupName(i);
        auto button = new ImageButton{mGroupButtonContainer, group, false};
        button->set_font_size(15);
        button->setId(i + 1);

        button->setSelectedCallback([this, group]() { selectGroup(group); });
    }

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Setting the filter again makes sure, that groups are correctly filtered.
    setFilter(mFilter->value());
    updateLayout();

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
        mGroupButtonContainer->set_position(
            nanogui::Vector2i{
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

    size_t id = (size_t)max(0, imageId(image));

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

    auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);

    float minimum = numeric_limits<float>::max();
    float maximum = numeric_limits<float>::min();
    for (const auto& channelName : channels) {
        const auto& channel = mCurrentImage->channel(channelName);
        auto [cmin, cmax, cmean] = channel->minMaxMean();
        maximum = max(maximum, cmax);
        minimum = min(minimum, cmin);
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

nanogui::Vector2i ImageViewer::sizeToFitImage(const shared_ptr<Image>& image) {
    if (!image) {
        return m_size;
    }

    nanogui::Vector2i requiredSize{image->displaySize().x(), image->displaySize().y()};

    // Convert from image pixel coordinates to nanogui coordinates.
    requiredSize = nanogui::Vector2i{nanogui::Vector2f{requiredSize} / pixel_ratio()};

    // Take into account the size of the UI.
    if (mSidebar->visible()) {
        requiredSize.x() += mSidebar->fixed_width();
    }

    if (mFooter->visible()) {
        requiredSize.y() += mFooter->fixed_height();
    }

    return requiredSize;
}

nanogui::Vector2i ImageViewer::sizeToFitAllImages() {
    nanogui::Vector2i result = m_size;
    for (const auto& image : mImages) {
        result = max(result, sizeToFitImage(image));
    }
    return result;
}

void ImageViewer::resizeToFit(nanogui::Vector2i targetSize) {
    // Only increase our current size if we are larger than the current size of the window.
    targetSize = max(m_size, targetSize);
    if (targetSize == m_size) {
        return;
    }

    // For sanity, don't make us larger than 8192x8192 to ensure that we don't break any texture size limitations of the user's GPU.
    auto maxSize = mMaxSize;

#ifdef _WIN32
    int padding = 2;
    maxSize.x() -= 2 * padding;
    maxSize.y() -= 2 * padding;
#endif

    targetSize = min(targetSize, maxSize);

    auto sizeDiff = targetSize - m_size;

    set_size(targetSize);
    move_window(-nanogui::Vector2i{sizeDiff.x() / 2, sizeDiff.y() / 2});

#ifdef _WIN32
    Vector2i pos;
    glfwGetWindowPos(m_glfw_window, &pos.x(), &pos.y());
    pos = nanogui::min(nanogui::max(pos, Vector2i{padding}), mMaxSize - targetSize - Vector2i{padding});
    glfwSetWindowPos(m_glfw_window, pos.x(), pos.y());
#endif

    if (autoFitToScreen() && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::playingBack() const { return mPlayButton->pushed(); }

void ImageViewer::setPlayingBack(bool value) {
    mPlayButton->set_pushed(value);
    mLastPlaybackFrameTime = chrono::steady_clock::now();
    redraw();
}

bool ImageViewer::setFilter(string_view filter) {
    mFilter->set_value(string{filter});
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

void ImageViewer::maximize() {
    glfwMaximizeWindow(m_glfw_window);
    if (autoFitToScreen() && mCurrentImage) {
        mImageCanvas->fitImageToScreen(*mCurrentImage);
    }
}

bool ImageViewer::isMaximized() { return glfwGetWindowAttrib(m_glfw_window, GLFW_MAXIMIZED) != 0; }

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
        mHelpWindow = new HelpWindow{this, mSupportsHdr, ipc(), [this] { toggleHelpWindow(); }};
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
            mImageInfoWindow = new ImageInfoWindow{this, mCurrentImage, mSupportsHdr, [this] { toggleImageInfoWindow(); }};
            mImageInfoWindow->center();
            mImageInfoWindow->request_focus();

            mImageInfoButton->set_pushed(true);
        }
    }

    requestLayoutUpdate();
}

void ImageViewer::updateImageInfoWindow() {
    if (mImageInfoWindow) {
        auto pos = mImageInfoWindow->position();
        auto size = mImageInfoWindow->size();
        mImageInfoWindow->dispose();

        if (mCurrentImage) {
            mImageInfoWindow = new ImageInfoWindow{this, mCurrentImage, mSupportsHdr, [this] { toggleImageInfoWindow(); }};
            mImageInfoWindow->set_position(pos);
            mImageInfoWindow->set_size(size);
            mImageInfoWindow->request_focus();

            mImageInfoButton->set_pushed(true);
        } else {
            mImageInfoWindow = nullptr;
            mImageInfoButton->set_pushed(false);
        }
    }
}

void ImageViewer::openImageDialog() {
    vector<string> paths = file_dialog(
        {
            {"apng", "Animated PNG image"               },
#ifdef TEV_SUPPORT_AVIF
            {"avif", "AV1 Image File"                   },
#endif
            {"bmp",  "Bitmap image"                     },
#ifdef _WIN32
            {"dds",  "DirectDraw Surface image"         },
#endif
            {"dng",  "Digital Negative image"           },
            {"exr",  "OpenEXR image"                    },
            {"gif",  "Graphics Interchange Format image"},
            {"hdr",  "HDR image"                        },
#ifdef TEV_SUPPORT_HEIC
            {"heic", "High Efficiency Image Container"  },
#endif
            {"jpeg", "JPEG image"                       },
            {"jpg",  "JPEG image"                       },
            {"jxl",  "JPEG-XL image"                    },
            {"pfm",  "Portable Float Map image"         },
            {"pgm",  "Portable GrayMap image"           },
            {"pic",  "PIC image"                        },
            {"png",  "Portable Network Graphics image"  },
            {"pnm",  "Portable AnyMap image"            },
            {"ppm",  "Portable PixMap image"            },
            {"psd",  "PSD image"                        },
            {"qoi",  "Quite OK Image format"            },
            {"tga",  "Truevision TGA image"             },
            {"tiff", "Tag Image File Format image"      },
            {"tif",  "Tag Image File Format image"      },
            {"webp", "WebP image"                       },
    },
        false,
        true
    );

    for (size_t i = 0; i < paths.size(); ++i) {
        bool shallSelect = i == paths.size() - 1;
        mImagesLoader->enqueue(toPath(paths[i]), "", shallSelect);
    }

    // Make sure we gain focus after seleting a file to be loaded.
    focusWindow();
}

void ImageViewer::saveImageDialog() {
    if (!mCurrentImage) {
        return;
    }

    fs::path path = toPath(file_dialog(
        {
            {"exr",  "OpenEXR image"                  },
            {"hdr",  "HDR image"                      },
            {"bmp",  "Bitmap Image File"              },
            {"jpg",  "JPEG image"                     },
            {"jpeg", "JPEG image"                     },
            {"jxl",  "JPEG-XL image"                  },
            {"png",  "Portable Network Graphics image"},
            {"qoi",  "Quite OK Image format"          },
            {"tga",  "Truevision TGA image"           },
    },
        true
    ));

    if (path.empty()) {
        return;
    }

    try {
        mImageCanvas->saveImage(path);
    } catch (const ImageSaveError& e) {
        new MessageDialog(this, MessageDialog::Type::Warning, "Error", fmt::format("Failed to save image: {}", e.what()));
    }

    // Make sure we gain focus after selecting a file to be loaded.
    focusWindow();
}

void throwIfNoCopyPasteCommand(bool copy) {
    string_view command = glfwGetPlatform() == GLFW_PLATFORM_X11 ? "xclip" : (copy ? "wl-copy" : "wl-paste");
    string_view copyPackage = glfwGetPlatform() == GLFW_PLATFORM_X11 ? "xclip" : "wl-clipboard";

    if (!commandExists(command)) {
        throw std::runtime_error{fmt::format("{} command not found. Install {} to copy to clipboard.", command, copyPackage)};
    }
}

void ImageViewer::copyImageCanvasToClipboard() const {
    if (!mCurrentImage) {
        throw std::runtime_error{"No image selected for copy."};
    }

    auto imageSize = mImageCanvas->imageDataSize();
    if (imageSize.x() == 0 || imageSize.y() == 0) {
        throw std::runtime_error{"Image canvas has no image data to copy to clipboard."};
    }

    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND || glfwGetPlatform() == GLFW_PLATFORM_X11) {
        throwIfNoCopyPasteCommand(true);

        auto imageData = mImageCanvas->getLdrImageData(true, std::numeric_limits<int>::max());
        auto pngImageSaver = make_unique<StbiLdrImageSaver>();

        auto stream = execw(glfwGetPlatform() == GLFW_PLATFORM_X11 ? "xclip -sel c -i -t image/png" : "wl-copy --type image/png");
        try {
            pngImageSaver->save(*stream, "clipboard.png", imageData, imageSize, 4);
        } catch (const ImageSaveError& e) {
            throw std::runtime_error{fmt::format("Failed to save image data to clipboard as PNG: {}", e.what())};
        }
    } else {
        clip::image_spec imageMetadata;
        imageMetadata.width = imageSize.x();
        imageMetadata.height = imageSize.y();
        imageMetadata.bits_per_pixel = 32;
        imageMetadata.bytes_per_row = imageMetadata.bits_per_pixel / 8 * imageMetadata.width;

        imageMetadata.red_mask = 0x000000ff;
        imageMetadata.green_mask = 0x0000ff00;
        imageMetadata.blue_mask = 0x00ff0000;
        imageMetadata.alpha_mask = 0xff000000;
        imageMetadata.red_shift = 0;
        imageMetadata.green_shift = 8;
        imageMetadata.blue_shift = 16;
        imageMetadata.alpha_shift = 24;

        auto imageData = mImageCanvas->getLdrImageData(true, std::numeric_limits<int>::max());
        clip::image image(imageData.data(), imageMetadata);

        if (!clip::set_image(image)) {
            throw std::runtime_error{"clip::set_image failed."};
        }
    }

    tlog::success() << "Image copied to clipboard.";
}

void ImageViewer::copyImageNameToClipboard() const {
    if (!mCurrentImage) {
        throw std::runtime_error{"No image selected for copy."};
    }

    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND || glfwGetPlatform() == GLFW_PLATFORM_X11) {
        throwIfNoCopyPasteCommand(true);

        auto stream = execw(glfwGetPlatform() == GLFW_PLATFORM_X11 ? "xclip -sel c -i -t text/plain" : "wl-copy --type text/plain");
        (*stream) << mCurrentImage->name();
    } else {
        if (!clip::set_text(string{mCurrentImage->name()})) {
            throw std::runtime_error{"clip::set_text failed."};
        }
    }

    tlog::success() << "Image path copied to clipboard.";
}

void ImageViewer::pasteImagesFromClipboard() {
    unique_ptr<istream> imageStream;
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND || glfwGetPlatform() == GLFW_PLATFORM_X11) {
        throwIfNoCopyPasteCommand(false);

        auto typesStream = execr(glfwGetPlatform() == GLFW_PLATFORM_X11 ? "xclip -sel c -o -t TARGETS" : "wl-paste --list-types");
        string imageTypes(std::istreambuf_iterator<char>(*typesStream), {});
        set<string_view> imageTypesSet;
        for (const auto& line : split(imageTypes, "\n")) {
            if (line.empty()) {
                continue;
            }

            tlog::debug() << fmt::format("Clipboard image type: {}", line);
            imageTypesSet.insert(line);
        }

        string selectedType = "";
        for (const auto& type : ImageLoader::supportedMimeTypes()) {
            if (imageTypesSet.contains(type)) {
                selectedType = type;
                break;
            }
        }

        if (selectedType.empty()) {
            throw std::runtime_error{"No image data found in clipboard."};
        }

        imageStream = execr(
            glfwGetPlatform() == GLFW_PLATFORM_X11 ? fmt::format("xclip -sel c -o -t {}", selectedType) :
                                                     fmt::format("wl-paste --type {}", selectedType)
        );
    } else {
        if (!clip::has(clip::image_format())) {
            throw std::runtime_error{"No image data found in clipboard."};
        }

        clip::image clipImage;
        if (!clip::get_image(clipImage)) {
            throw std::runtime_error{"clip::get_image failed."};
        }

        auto ss = make_unique<stringstream>();
        (*ss) << "clip";
        ss->write(reinterpret_cast<const char*>(&clipImage.spec()), sizeof(clip::image_spec));
        ss->write(clipImage.data(), clipImage.spec().bytes_per_row * clipImage.spec().height);
        imageStream = std::move(ss);
    }

    tlog::info() << "Loading image from clipboard...";
    auto imagesLoadTask = tryLoadImage(
        fmt::format("clipboard ({})", ++mClipboardIndex), *imageStream, "", mImagesLoader->applyGainmaps(), mImagesLoader->groupChannels()
    );

    const auto images = imagesLoadTask.get();

    if (images.empty()) {
        throw std::runtime_error{"Failed to load image from clipboard data."};
    } else {
        for (auto& image : images) {
            addImage(image, true);
        }
    }
}

void ImageViewer::showErrorDialog(string_view message) {
    tlog::error() << message;
    new MessageDialog(this, MessageDialog::Type::Warning, "Error", string{message});
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
    mImageCanvas->set_fixed_size(m_size - nanogui::Vector2i{sidebarWidth, footerHeight});
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
    mHelpButton->set_position(nanogui::Vector2i{mSidebarLayout->fixed_width() - 38, 5});
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
    string caption = "tev";
    if (mCurrentImage) {
        auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);
        // Remove duplicates
        channels.erase(unique(begin(channels), end(channels)), end(channels));

        auto channelTails = channels;
        transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);

        caption = fmt::format("{} – {} – {}%", mCurrentImage->shortName(), mCurrentGroup, (int)std::round(mImageCanvas->scale() * 100));

        auto rel = mouse_pos() - mImageCanvas->position();
        vector<float> values = mImageCanvas->getValuesAtNanoPos({rel.x(), rel.y()}, channels);
        nanogui::Vector2i imageCoords = mImageCanvas->getImageCoords(mCurrentImage.get(), {rel.x(), rel.y()});
        TEV_ASSERT(values.size() >= channelTails.size(), "Should obtain a value for every existing channel.");

        string valuesString;
        for (size_t i = 0; i < channelTails.size(); ++i) {
            valuesString += fmt::format("{:.2f},", values[i]);
        }
        valuesString.pop_back();
        valuesString += " / 0x";
        for (size_t i = 0; i < channelTails.size(); ++i) {
            float tonemappedValue = channelTails[i] == "A" ? values[i] : toSRGB(values[i]);
            unsigned char discretizedValue = (char)(tonemappedValue * 255 + 0.5f);
            valuesString += fmt::format("{:02X}", discretizedValue);
        }

        caption += fmt::format(
            " – @{},{} ({:.3f},{:.3f}) / {}x{}: {}",
            imageCoords.x(),
            imageCoords.y(),
            imageCoords.x() / (double)mCurrentImage->size().x(),
            imageCoords.y() / (double)mCurrentImage->size().y(),
            mCurrentImage->size().x(),
            mCurrentImage->size().y(),
            valuesString
        );
    }

    set_caption(caption);
}

string ImageViewer::groupName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    const auto groups = mCurrentImage->channelGroups();
    TEV_ASSERT(index < groups.size(), "Group index out of bounds.");
    return groups[index].name;
}

int ImageViewer::groupId(string_view groupName) const {
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

int ImageViewer::imageId(string_view imageName) const {
    auto pos = static_cast<size_t>(distance(begin(mImages), find_if(begin(mImages), end(mImages), [&](const shared_ptr<Image>& image) {
                                                return image->name() == imageName;
                                            })));
    return pos >= mImages.size() ? -1 : (int)pos;
}

string ImageViewer::nextGroup(string_view group, EDirection direction) {
    if (mGroupButtonContainer->child_count() == 0) {
        return mCurrentGroup;
    }

    int dir = direction == Forward ? 1 : -1;

    // If the group does not exist, start at index 0.
    int startId = max(0, groupId(group));

    int id = startId;
    do {
        id = (id + mGroupButtonContainer->child_count() + dir) % mGroupButtonContainer->child_count();
    } while (!mGroupButtonContainer->child_at(id)->visible() && id != startId);

    return groupName(id);
}

string ImageViewer::nthVisibleGroup(size_t n) {
    string lastVisible = mCurrentGroup;
    for (int i = 0; i < mGroupButtonContainer->child_count(); ++i) {
        if (mGroupButtonContainer->child_at(i)->visible()) {
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
        id = (id + mImageButtonContainer->child_count() + dir) % mImageButtonContainer->child_count();
    } while (!mImageButtonContainer->child_at(id)->visible() && id != startId);

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

shared_ptr<Image> ImageViewer::imageByName(string_view imageName) {
    int id = imageId(imageName);
    if (id != -1) {
        return mImages[id];
    } else {
        return nullptr;
    }
}

} // namespace tev
