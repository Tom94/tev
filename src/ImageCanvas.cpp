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

#include <tev/Common.h>
#include <tev/FalseColor.h>
#include <tev/ImageCanvas.h>
#include <tev/ThreadPool.h>

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/vector.h>

#include <chrono>
#include <set>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

ImageCanvas::ImageCanvas(Widget* parent) : Canvas{parent, 1, false, false, false} {
    // If we are rendering to a float buffer (which is the case if the screen has a float back buffer, or if the screen performs color
    // management), we don't need to dither here. The screen will do it. Otherwise, we are rendering directly to an integer buffer and we
    // *should* dither to avoid banding artifacts.
    auto* screen = this->screen();
    const float ditherScale = (screen->has_float_buffer() || screen->applies_color_management()) ?
        0.0f :
        (1.0f / (1u << screen->bits_per_sample()));

    mShader.reset(new UberShader{render_pass(), ditherScale});
    set_draw_border(false);
}

bool ImageCanvas::scroll_event(const Vector2i& p, const Vector2f& rel) {
    if (Canvas::scroll_event(p, rel)) {
        return true;
    }

    float scaleAmount = rel.y();
    auto* glfwWindow = screen()->glfw_window();
    // There is no explicit access to the currently pressed modifier keys here, so we need to directly ask GLFW.
    if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
        scaleAmount /= 8;
    } else if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL)) {
        scaleAmount *= 8;
    }

    scale(scaleAmount, Vector2f{p});
    return true;
}

void ImageCanvas::draw_contents() {
    auto* glfwWindow = screen()->glfw_window();
    bool viewReferenceOnly = glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT);
    bool viewImageOnly = glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL);
    if (viewReferenceOnly && viewImageOnly) {
        // If both modifiers are pressed at the same time, we want entirely different behavior from modifying which image is shown. Do
        // nothing here.
        viewReferenceOnly = viewImageOnly = false;
    }

    Image* image = (mReference && viewReferenceOnly) ? mReference.get() : mImage.get();

    optional<Box2i> imageSpaceCrop = nullopt;
    if (image && mCrop.has_value()) {
        imageSpaceCrop = mCrop.value().translate(image->displayWindow().min - image->dataWindow().min);
    }

    viewImageOnly |= !mReference || image == mReference.get();

    Image* reference = (viewImageOnly || !mReference || image == mReference.get()) ? nullptr : mReference.get();

    mShader->draw(
        2.0f * inverse(Vector2f{m_size}) / mPixelRatio,
        Vector2f{20.0f},
        image,
        // The uber shader operates in [-1, 1] coordinates and requires the _inserve_ image transform to obtain texture coordinates in [0,
        // 1]-space.
        inverse(transform(mImage.get())),
        reference,
        inverse(transform(mReference.get())),
        mRequestedChannelGroup,
        mMinFilter,
        mMagFilter,
        mExposure,
        mOffset,
        mGamma,
        mWhiteLevelOverride ? (*mWhiteLevelOverride / glfwGetWindowSdrWhiteLevel(glfwWindow)) : 1.0f,
        mClipToLdr,
        mBackgroundColor,
        mTonemap,
        mMetric,
        imageSpaceCrop
    );
}

void ImageCanvas::drawPixelValuesAsText(NVGcontext* ctx) {
    TEV_ASSERT(mImage, "Can only draw pixel values if there exists an image.");

    auto texToNano = textureToNanogui(mImage.get());
    auto nanoToTex = inverse(texToNano);

    Vector2f pixelSize = texToNano * Vector2f{1.0f} - texToNano * Vector2f{0.0f};

    Vector2f topLeft = (nanoToTex * Vector2f{0.0f});
    Vector2f bottomRight = (nanoToTex * Vector2f{m_size});

    Vector2i startIndices = Vector2i{
        static_cast<int>(floor(topLeft.x())),
        static_cast<int>(floor(topLeft.y())),
    };

    Vector2i endIndices = Vector2i{
        static_cast<int>(ceil(bottomRight.x())),
        static_cast<int>(ceil(bottomRight.y())),
    };

    if (pixelSize.x() > 50 && pixelSize.x() < 1024) {
        vector<string> channels = mImage->channelsInGroup(mRequestedChannelGroup);
        // Remove duplicates
        channels.erase(unique(begin(channels), end(channels)), end(channels));
        if (channels.empty()) {
            return;
        }

        // Only treat alpha specially if it's not the only channel
        const bool hasAlpha = channels.size() > 1 && Channel::isAlpha(channels.back());

        vector<Color> colors;
        for (const auto& channel : channels) {
            colors.emplace_back(Channel::color(channel, true));
        }

        float fontSize = pixelSize.x() / 6;
        if (colors.size() > 4) {
            fontSize *= 4.0f / colors.size();
        }

        const float fontAlpha = std::min(std::min(1.0f, (pixelSize.x() - 50) / 30), (1024 - pixelSize.x()) / 256);

        nvgFontSize(ctx, fontSize);
        nvgFontFace(ctx, "sans");
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        auto* glfwWindow = screen()->glfw_window();
        bool shiftAndControlHeld = (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) &&
            (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_CONTROL));

        Vector2i cur;
        vector<float> values;
        for (cur.y() = startIndices.y(); cur.y() < endIndices.y(); ++cur.y()) {
            for (cur.x() = startIndices.x(); cur.x() < endIndices.x(); ++cur.x()) {
                Vector2i nano = Vector2i{texToNano * (Vector2f{cur} + Vector2f{0.5f})};
                getValuesAtNanoPos(nano, values, channels);

                TEV_ASSERT(values.size() == colors.size(), "Can not have more values than channels.");

                // If shift and control are held, we display sRGB hex values
                if (shiftAndControlHeld) {
                    for (size_t i = 0; i < colors.size(); ++i) {
                        values[i] = hasAlpha && i == colors.size() - 1 ? values[i] : toSRGB(values[i]);
                    }
                } else {
                    // Otherwise, display what the user configured as the inspection color space
                    applyInspectionParameters(values, hasAlpha);
                }

                for (size_t i = 0; i < colors.size(); ++i) {
                    string str;
                    Vector2f pos;

                    if (shiftAndControlHeld) {
                        const unsigned char discretizedValue = (char)(clamp(values[i], 0.0f, 1.0f) * 255 + 0.5f);
                        str = fmt::format("{:02X}", discretizedValue);
                        pos = Vector2f{
                            m_pos.x() + nano.x() + (i - 0.5f * (colors.size() - 1)) * fontSize * 0.88f,
                            (float)m_pos.y() + nano.y(),
                        };
                    } else {
                        str = abs(values[i]) > 100000 ? fmt::format("{:6g}", values[i]) : fmt::format("{:.5f}", values[i]);
                        pos = Vector2f{
                            (float)m_pos.x() + nano.x(),
                            m_pos.y() + nano.y() + (i - 0.5f * (colors.size() - 1)) * fontSize,
                        };
                    }

                    const Color col = colors[i];
                    nvgFillColor(ctx, Color(col.r(), col.g(), col.b(), fontAlpha));
                    drawTextWithShadow(ctx, pos.x(), pos.y(), str, fontAlpha);
                }
            }
        }
    }
}

void ImageCanvas::drawCoordinateSystem(NVGcontext* ctx) {
    TEV_ASSERT(mImage, "Can only draw coordinate system if there exists an image.");

    auto displayWindowToNano = displayWindowToNanogui(mImage.get());

    enum DrawFlags {
        Label = 1,
        Region = 2,
    };

    auto drawWindow = [&](Box2f window, Color color, bool top, bool right, string_view name, DrawFlags flags) {
        float fontSize = 20;
        float strokeWidth = 3.0f;

        Vector2i topLeft = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.min.x(), window.min.y()}
        };
        Vector2i topRight = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.max.x(), window.min.y()}
        };
        Vector2i bottomLeft = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.min.x(), window.max.y()}
        };
        Vector2i bottomRight = m_pos +
            Vector2i{
                displayWindowToNano * Vector2f{window.max.x(), window.max.y()}
        };

        nvgSave(ctx);

        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, fontSize);
        nvgTextAlign(ctx, (right ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) | (top ? NVG_ALIGN_BOTTOM : NVG_ALIGN_TOP));
        float textWidth = nvgTextBounds(ctx, 0, 0, name.data(), name.data() + name.size(), nullptr);
        float textAlpha = std::max(std::min(1.0f, (((topRight.x() - topLeft.x() - textWidth - 5) / 30))), 0.0f);
        float regionAlpha = std::max(std::min(1.0f, (((topRight.x() - topLeft.x() - textWidth - 5) / 30))), 0.0f);

        Color textColor = Color(190, 255);
        textColor.a() = textAlpha;

        if (flags & Region) {
            color.a() = regionAlpha;

            nvgBeginPath(ctx);
            nvgMoveTo(ctx, bottomLeft.x(), bottomLeft.y());
            nvgLineTo(ctx, topLeft.x(), topLeft.y());
            nvgLineTo(ctx, topRight.x(), topRight.y());
            nvgLineTo(ctx, bottomRight.x(), bottomRight.y());
            nvgLineTo(ctx, bottomLeft.x(), bottomLeft.y());
            nvgStrokeWidth(ctx, strokeWidth);
            nvgStrokeColor(ctx, color);
            nvgStroke(ctx);
        }

        if (!name.empty() && (flags & Label)) {
            color.a() = textAlpha;

            nvgBeginPath(ctx);
            nvgFillColor(ctx, color);

            float cornerRadius = fontSize / 3;
            float topLeftCornerRadius = top && right ? cornerRadius : 0;
            float topRightCornerRadius = top && !right ? cornerRadius : 0;
            float bottomLeftCornerRadius = !top && right ? cornerRadius : 0;
            float bottomRightCornerRadius = !top && !right ? cornerRadius : 0;

            nvgRoundedRectVarying(
                ctx,
                right ? (topRight.x() - textWidth - 4 * strokeWidth) : topLeft.x() - strokeWidth / 2,
                topLeft.y() - (top ? fontSize : 0),
                textWidth + 4 * strokeWidth,
                fontSize,
                topLeftCornerRadius,
                topRightCornerRadius,
                bottomRightCornerRadius,
                bottomLeftCornerRadius
            );
            nvgFill(ctx);

            nvgFillColor(ctx, textColor);
            nvgText(
                ctx,
                right ? (topRight.x() - 2 * strokeWidth) : (topLeft.x() + 2 * strokeWidth) - strokeWidth / 2,
                topLeft.y(),
                name.data(),
                name.data() + name.size()
            );
        }

        nvgRestore(ctx);
    };

    auto draw = [&](DrawFlags flags) {
        if (mReference) {
            if (mReference->dataWindow() != mImage->dataWindow()) {
                drawWindow(
                    mReference->dataWindow(),
                    REFERENCE_COLOR,
                    mReference->displayWindow().min.y() > mReference->dataWindow().min.y(),
                    true,
                    "Reference data window",
                    flags
                );
            }

            if (mReference->displayWindow() != mImage->displayWindow()) {
                drawWindow(
                    mReference->displayWindow(),
                    REFERENCE_COLOR,
                    mReference->displayWindow().min.y() <= mReference->dataWindow().min.y(),
                    true,
                    "Reference display window",
                    flags
                );
            }
        }

        if (mImage->dataWindow() != mImage->displayWindow()) {
            drawWindow(
                mImage->dataWindow(), IMAGE_COLOR, mImage->displayWindow().min.y() > mImage->dataWindow().min.y(), false, "Data window", flags
            );
            drawWindow(
                mImage->displayWindow(),
                Color(0.3f, 1.0f),
                mImage->displayWindow().min.y() <= mImage->dataWindow().min.y(),
                false,
                "Display window",
                flags
            );
        } else {
            drawWindow(
                mImage->displayWindow(), Color(0.3f, 1.0f), mImage->displayWindow().min.y() <= mImage->dataWindow().min.y(), false, "", flags
            );
        }

        if (mCrop.has_value()) {
            drawWindow(mCrop.value(), CROP_COLOR, false, false, "Crop", flags);
        }
    };

    // Draw all labels after the regions to ensure no occlusion
    draw(Region);
    draw(Label);
}

void ImageCanvas::drawEdgeShadows(NVGcontext* ctx) {
    int ds = m_theme->m_window_drop_shadow_size, cr = m_theme->m_window_corner_radius;
    NVGpaint shadowPaint =
        nvgBoxGradient(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y(), cr * 2, ds * 2, m_theme->m_transparent, m_theme->m_drop_shadow);

    nvgSave(ctx);
    nvgResetScissor(ctx);
    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgRoundedRect(ctx, m_pos.x() + ds, m_pos.y() + ds, m_size.x() - 2 * ds, m_size.y() - 2 * ds, cr);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, shadowPaint);
    nvgFill(ctx);
    nvgRestore(ctx);
}

void ImageCanvas::draw(NVGcontext* ctx) {
    Canvas::draw(ctx);

    if (mImage) {
        drawPixelValuesAsText(ctx);

        auto displayWindowToNano = displayWindowToNanogui(mImage.get());

        auto vgToNano = [&](const Vector2f& p) { return Vector2f{m_pos} + displayWindowToNano * p; };

        auto applyVgCommand = [&](const VgCommand& command) {
            const float* f = command.data.data();
            switch (command.type) {
                // State
                case VgCommand::EType::Save: nvgSave(ctx); return;
                case VgCommand::EType::Restore: nvgRestore(ctx); return;
                // Draw calls
                case VgCommand::EType::FillColor: nvgFillColor(ctx, {{{f[0], f[1], f[2], f[3]}}}); return;
                case VgCommand::EType::Fill: nvgFill(ctx); return;
                case VgCommand::EType::StrokeColor: nvgStrokeColor(ctx, {{{f[0], f[1], f[2], f[3]}}}); return;
                case VgCommand::EType::Stroke: nvgStroke(ctx); return;
                // Path control
                case VgCommand::EType::BeginPath: nvgBeginPath(ctx); return;
                case VgCommand::EType::ClosePath: nvgClosePath(ctx); return;
                case VgCommand::EType::PathWinding: nvgPathWinding(ctx, (int)f[0]); return;
                case VgCommand::EType::DebugDumpPathCache: nvgDebugDumpPathCache(ctx); return;
                // Path construction
                case VgCommand::EType::MoveTo: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    nvgMoveTo(ctx, p.x(), p.y());
                }
                    return;
                case VgCommand::EType::LineTo: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    nvgLineTo(ctx, p.x(), p.y());
                }
                    return;
                case VgCommand::EType::ArcTo: {
                    Vector2f p1 = vgToNano({f[0], f[1]});
                    Vector2f p2 = vgToNano({f[2], f[3]});
                    float radius = f[4] * extractScale(displayWindowToNano);
                    nvgArcTo(ctx, p1.x(), p1.y(), p2.x(), p2.y(), radius);
                }
                    return;
                case VgCommand::EType::Arc: {
                    Vector2f c = vgToNano({f[0], f[1]});
                    float radius = f[2] * extractScale(displayWindowToNano);
                    nvgArc(ctx, c.x(), c.y(), radius, f[3], f[4], (int)f[5]);
                }
                    return;
                case VgCommand::EType::BezierTo: {
                    Vector2f c1 = vgToNano({f[0], f[1]});
                    Vector2f c2 = vgToNano({f[2], f[3]});
                    Vector2f p = vgToNano({f[4], f[5]});
                    nvgBezierTo(ctx, c1.x(), c1.y(), c2.x(), c2.y(), p.x(), p.y());
                }
                    return;
                case VgCommand::EType::Circle: {
                    Vector2f c = vgToNano({f[0], f[1]});
                    float radius = f[2] * extractScale(displayWindowToNano);
                    nvgCircle(ctx, c.x(), c.y(), radius);
                }
                    return;
                case VgCommand::EType::Ellipse: {
                    Vector2f c = vgToNano({f[0], f[1]});
                    Vector2f r = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    nvgEllipse(ctx, c.x(), c.y(), r.x(), r.y());
                }
                    return;
                case VgCommand::EType::QuadTo:

                {
                    Vector2f c = vgToNano({f[0], f[1]});
                    Vector2f p = vgToNano({f[2], f[3]});
                    nvgQuadTo(ctx, c.x(), c.y(), p.x(), p.y());
                }

                    return;
                case VgCommand::EType::Rect: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    Vector2f size = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    nvgRect(ctx, p.x(), p.y(), size.x(), size.y());
                }
                    return;
                case VgCommand::EType::RoundedRect: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    Vector2f size = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    float radius = f[4] * extractScale(displayWindowToNano);
                    nvgRoundedRect(ctx, p.x(), p.y(), size.x(), size.y(), radius);
                }
                    return;
                case VgCommand::EType::RoundedRectVarying: {
                    Vector2f p = vgToNano({f[0], f[1]});
                    Vector2f size = extract2x2(displayWindowToNano) * Vector2f{f[2], f[3]};
                    float scale = extractScale(displayWindowToNano);
                    nvgRoundedRectVarying(ctx, p.x(), p.y(), size.x(), size.y(), f[4] * scale, f[5] * scale, f[6] * scale, f[7] * scale);
                }
                    return;
                // TODO: text rendering
                default: throw runtime_error{"Invalid VgCommand type."};
            }
        };

        // Draw image-specific vector graphics overlay for both the currently selected image as well as the reference.
        auto applyVgCommandsSandboxed = [&](const Color& defaultColor, span<const VgCommand> commands) {
            nvgSave(ctx);

            nvgFillColor(ctx, defaultColor);
            nvgStrokeColor(ctx, defaultColor);
            nvgStrokeWidth(ctx, 3.0f);

            size_t saveCounter = 0;
            for (const auto& command : commands) {
                if (command.type == VgCommand::EType::Save) {
                    ++saveCounter;
                } else if (command.type == VgCommand::EType::Restore) {
                    if (saveCounter == 0) {
                        tlog::warning() << "Malformed vector graphics commands: restore before save";
                        continue;
                    }

                    --saveCounter;
                }

                applyVgCommand(command);
            }

            if (saveCounter > 0) {
                tlog::warning() << "Malformed vector graphics commands: missing restore after save";
                for (size_t i = 0; i < saveCounter; ++i) {
                    nvgRestore(ctx);
                }
            }

            nvgRestore(ctx);
        };

        if (mReference && !mReference->vgCommands().empty()) {
            applyVgCommandsSandboxed(REFERENCE_COLOR, mReference->vgCommands());
        }

        if (!mImage->vgCommands().empty()) {
            applyVgCommandsSandboxed(IMAGE_COLOR, mImage->vgCommands());
        }

        // If the coordinate system is in any sort of way non-trivial, or if a hotkey is held, draw it!
        if (glfwGetKey(screen()->glfw_window(), GLFW_KEY_B) || mCrop.has_value() || mImage->dataWindow() != mImage->displayWindow() ||
            mImage->displayWindow().min != Vector2i{0} ||
            (mReference && (mReference->dataWindow() != mImage->dataWindow() || mReference->displayWindow() != mImage->displayWindow()))) {
            drawCoordinateSystem(ctx);
        }
    }

    // If we're not in fullscreen mode draw an inner drop shadow. (adapted from Window)
    if (m_pos.x() != 0) {
        drawEdgeShadows(ctx);
    }
}

void ImageCanvas::translate(const Vector2f& amount) { mTransform = Matrix3f::translate(amount) * mTransform; }

void ImageCanvas::scale(float amount, const Vector2f& origin) {
    static const double BASE_SCALE = sqrt(sqrt(sqrt(2.0)));
    const float scaleFactor = (float)pow(BASE_SCALE, (double)amount);

    // Use the current cursor position as the origin to scale around.
    Vector2f offset = -(origin - Vector2f{position()}) + 0.5f * Vector2f{m_size};
    auto scaleTransform = Matrix3f::translate(-offset) * Matrix3f::scale(Vector2f{scaleFactor}) * Matrix3f::translate(offset);

    mTransform = scaleTransform * mTransform;
}

Vector2i ImageCanvas::getImageCoords(const Image* image, Vector2i nanoPos) {
    Vector2f imagePos = inverse(textureToNanogui(image)) * Vector2f{nanoPos};
    return {
        static_cast<int>(floor(imagePos.x())),
        static_cast<int>(floor(imagePos.y())),
    };
}

Vector2i ImageCanvas::getDisplayWindowCoords(const Image* image, Vector2i nanoPos) {
    Vector2f imageCoords = getImageCoords(image, nanoPos);
    if (image) {
        imageCoords += Vector2f(image->dataWindow().min - image->displayWindow().min);
    }

    return imageCoords;
}

void ImageCanvas::getValuesAtNanoPos(Vector2i nanoPos, vector<float>& result, span<const string> channels) {
    result.clear();
    if (!mImage) {
        return;
    }

    auto imageCoords = getImageCoords(mImage.get(), nanoPos);
    for (const auto& channel : channels) {
        const Channel* c = mImage->channel(channel);
        TEV_ASSERT(c, "Requested channel must exist.");
        result.push_back(c->eval(imageCoords));
    }

    // Subtract reference if it exists.
    if (mReference) {
        const auto referenceCoords = getImageCoords(mReference.get(), nanoPos);
        for (size_t i = 0; i < result.size(); ++i) {
            const bool isAlpha = Channel::isAlpha(channels[i]);
            const float defaultVal = isAlpha && mReference->contains(referenceCoords) ? 1.0f : 0.0f;

            const Channel* c = mReference->channel(channels[i]);
            const float reference = c ? c->eval(referenceCoords) : defaultVal;

            result[i] = isAlpha ? 0.5f * (result[i] + reference) : applyMetric(result[i], reference, mMetric);
        }
    }
}

Box2i ImageCanvas::cropInImageCoords() const {
    if (!mImage) {
        return Box2i{0};
    }

    const auto region = mImage->toImageCoords(mCrop.has_value() ? *mCrop : mImage->displayWindow());
    if (!region.isValid()) {
        return Box2i{0};
    }

    return region;
}

void ImageCanvas::fitImageToScreen(const Image& image) {
    Vector2f nanoguiImageSize = Vector2f{image.displayWindow().size()} / mPixelRatio;
    mTransform = Matrix3f::scale(Vector2f{std::min(m_size.x() / nanoguiImageSize.x(), m_size.y() / nanoguiImageSize.y())});
}

void ImageCanvas::resetTransform() { mTransform = Matrix3f::scale(Vector2f{1.0f}); }

Task<HeapArray<float>> ImageCanvas::getRgbaHdrImageData(bool divideAlpha, int priority) const {
    if (!mImage) {
        co_return {};
    }

    co_return co_await mImage->getRgbaHdrImageData(
        mReference, cropInImageCoords(), mRequestedChannelGroup, mMetric, mBackgroundColor, divideAlpha, priority
    );
}

Task<HeapArray<uint8_t>> ImageCanvas::getRgbaLdrImageData(bool divideAlpha, int priority) const {
    if (!mImage) {
        co_return {};
    }

    co_return co_await mImage->getRgbaLdrImageData(
        mReference, cropInImageCoords(), mRequestedChannelGroup, mMetric, mBackgroundColor, divideAlpha, mTonemap, mGamma, mExposure, mOffset, priority
    );
}

void ImageCanvas::saveImage(const fs::path& path) const {
    if (!mImage) {
        throw ImageSaveError{"There is no image to save."};
    }

    tlog::info() << "Saving currently displayed image as " << path << ".";

    const auto start = chrono::steady_clock::now();

    mImage
        ->save(
            path,
            mReference,
            cropInImageCoords(),
            mRequestedChannelGroup,
            mMetric,
            mBackgroundColor,
            mTonemap,
            mGamma,
            mExposure,
            mOffset,
            numeric_limits<int>::max() // Use maximum priority for saving images.
        )
        .get();

    const auto elapsedSeconds = chrono::duration<double>{chrono::steady_clock::now() - start};
    tlog::success() << fmt::format("Saved {} after {:.3f} seconds.", path, elapsedSeconds.count());
}

shared_ptr<Lazy<shared_ptr<CanvasStatistics>>> ImageCanvas::canvasStatistics() {
    if (!mImage) {
        return nullptr;
    }

    const string channels = join(mImage->channelsInGroup(mRequestedChannelGroup), ",");

    ostringstream keyStream;
    keyStream << fmt::format(
        "{}-{}-{}-{}-{}-{}",
        (int)mInspectionTransfer,
        mInspectionChroma,
        mInspectionAdaptWhitePoint,
        mInspectionPremultipliedAlpha,
        mImage->id(),
        channels
    );

    if (mReference) {
        keyStream << fmt::format("-{}-{}", mReference->id(), (int)mMetric);
    }

    if (mCrop.has_value()) {
        keyStream << fmt::format("-crop-{}-{}", mCrop->min, mCrop->max);
    }

    const string key = keyStream.str();

    const auto iter = mCanvasStatistics.find(key);
    if (iter != end(mCanvasStatistics)) {
        return iter->second;
    }

    promise<shared_ptr<CanvasStatistics>> promise;
    mCanvasStatistics.insert(make_pair(key, make_shared<Lazy<shared_ptr<CanvasStatistics>>>(promise.get_future())));

    // Remember the keys associateed with the participating images. Such that their canvas statistics can be retrieved and deleted when
    // either of the images is closed or mutated.
    mImageIdToCanvasStatisticsKey[mImage->id()].emplace_back(key);
    mImage->setStaleIdCallback([this](int id) { purgeCanvasStatistics(id); });

    if (mReference) {
        mImageIdToCanvasStatisticsKey[mReference->id()].emplace_back(key);
        mReference->setStaleIdCallback([this](int id) { purgeCanvasStatistics(id); });
    }

    // Later requests must have higher priority than previous ones.
    static atomic<int> sId{0};
    invokeTaskDetached(
        [image = mImage,
         reference = mReference,
         requestedChannelGroup = mRequestedChannelGroup,
         metric = mMetric,
         region = cropInImageCoords(),
         chroma = mInspectionChroma,
         transfer = mInspectionTransfer,
         adaptWhitePoint = mInspectionAdaptWhitePoint,
         premultipliedAlpha = mInspectionPremultipliedAlpha,
         priority = ++sId,
         p = std::move(promise)]() mutable -> Task<void> {
            co_await ThreadPool::global().enqueueCoroutine(priority);
            p.set_value(
                co_await computeCanvasStatistics(
                    image, reference, requestedChannelGroup, metric, region, chroma, transfer, adaptWhitePoint, premultipliedAlpha, priority
                )
            );
            redrawWindow();
        }
    );

    return mCanvasStatistics.at(key);
}

void ImageCanvas::purgeCanvasStatistics(int imageId) {
    for (const auto& key : mImageIdToCanvasStatisticsKey[imageId]) {
        mCanvasStatistics.erase(key);
    }

    mImageIdToCanvasStatisticsKey.erase(imageId);
}

void ImageCanvas::applyInspectionParameters(vector<float>& values, bool hasAlpha) {
    if (values.empty()) {
        return;
    }

    // If we have 3 color channels, apply alpha, the inspection chroma, and transfer function
    const size_t nColorChannels = values.size() - (hasAlpha ? 1 : 0);

    const float alpha = hasAlpha && !mInspectionPremultipliedAlpha ? values.back() : 1.0f;
    const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

    if (nColorChannels >= 3) {
        const auto mat = convertColorspaceMatrix(
            rec709Chroma(),
            mInspectionChroma,
            mInspectionAdaptWhitePoint ? ERenderingIntent::RelativeColorimetric : ERenderingIntent::AbsoluteColorimetric
        );

        Vector3f rgb;
        for (size_t c = 0; c < 3; ++c) {
            rgb[c] = values[c] * alphaFactor;
        }

        rgb = ituth273::transfer(mInspectionTransfer, mat * rgb);
        for (size_t c = 0; c < 3; ++c) {
            values[c] = rgb[c];
        }
    } else {
        // Otherwise, apply just alpha and transfer function
        for (size_t c = 0; c < nColorChannels; ++c) {
            values[c] = ituth273::transferComponent(mInspectionTransfer, values[c] * alphaFactor);
        }
    }
}

Task<shared_ptr<CanvasStatistics>> ImageCanvas::computeCanvasStatistics(
    shared_ptr<Image> image,
    shared_ptr<Image> reference,
    string_view requestedChannelGroup,
    EMetric metric,
    Box2i region,
    const chroma_t& chroma,
    ituth273::ETransfer transfer,
    bool adaptWhitePoint,
    bool premultipliedAlpha,
    int priority
) {
    TEV_ASSERT(image, "Image must be valid.");

    // If the crop region is outside the image, intersect. If no intersection, use the empty region about the origin.
    region = region.intersect(Box2i{image->size()});
    if (!region.isValid()) {
        region = Box2i{0};
    }

    const auto start = chrono::steady_clock::now();
    const auto scopeGuard = ScopeGuard([&]() {
        const auto end = chrono::steady_clock::now();
        const chrono::duration<double> elapsedSeconds = end - start;
        tlog::debug() << fmt::format("Computed canvas statistics for {} in {:.4f} seconds.", image->name(), elapsedSeconds.count());
    });

    auto flattened = co_await image->getHdrImageData(reference, requestedChannelGroup, metric, priority);

    const Channel* alphaChannel = nullptr;

    // Only treat the alpha channel specially if it is not the only channel of the image.
    if (!all_of(begin(flattened), end(flattened), [](const Channel& c) { return c.isAlpha(); })) {
        if (flattened.back().isAlpha()) {
            alphaChannel = &flattened.back();
        }
    }

    const auto result = make_shared<CanvasStatistics>();
    const size_t nColorChannels = alphaChannel ? (flattened.size() - 1) : flattened.size();
    result->nChannels = (int)nColorChannels;

    result->histogramColors.resize(nColorChannels);
    for (size_t i = 0; i < nColorChannels; ++i) {
        string rgba[] = {"R", "G", "B", "A"};
        string colorName = nColorChannels == 1 ? "L" : rgba[std::min(i, (size_t)3)];
        result->histogramColors[i] = Channel::color(colorName, false);
    }

    const auto regionSize = region.size();
    const size_t numPixels = region.area();
    const size_t numSamples = nColorChannels * numPixels;

    // If we have 3 color channels, apply alpha, the inspection chroma, and transfer function
    if (nColorChannels >= 3) {
        const auto mat = convertColorspaceMatrix(
            rec709Chroma(), chroma, adaptWhitePoint ? ERenderingIntent::RelativeColorimetric : ERenderingIntent::AbsoluteColorimetric
        );

        co_await ThreadPool::global().parallelForAsync<int>(
            region.min.y(),
            region.max.y(),
            numSamples,
            [&](int y) {
                for (int x = region.min.x(); x < region.max.x(); ++x) {
                    const float alpha = alphaChannel && !premultipliedAlpha ? alphaChannel->at({x, y}) : 1.0f;
                    const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

                    Vector3f rgb;
                    for (size_t c = 0; c < 3; ++c) {
                        rgb[c] = flattened[c].at({x, y}) * alphaFactor;
                    }

                    rgb = ituth273::transfer(transfer, mat * rgb);
                    for (size_t c = 0; c < 3; ++c) {
                        flattened[c].setAt({x, y}, rgb[c]);
                    }
                }
            },
            priority
        );
    } else {
        // Otherwise, apply just alpha and transfer function
        if (transfer != ituth273::ETransfer::Linear || (alphaChannel && !premultipliedAlpha)) {
            co_await ThreadPool::global().parallelForAsync<int>(
                region.min.y(),
                region.max.y(),
                numSamples,
                [&](int y) {
                    for (int x = region.min.x(); x < region.max.x(); ++x) {
                        const float alpha = alphaChannel && !premultipliedAlpha ? alphaChannel->at({x, y}) : 1.0f;
                        const float alphaFactor = alpha == 0 ? 0.0f : 1.0f / alpha;

                        for (size_t c = 0; c < nColorChannels; ++c) {
                            auto& channel = flattened[c];
                            const float val = channel.at({x, y}) * alphaFactor;
                            channel.setAt({x, y}, ituth273::transferComponent(transfer, val));
                        }
                    }
                },
                priority
            );
        }
    }

    struct Stats {
        double mean = 0;
        float maximum = -numeric_limits<float>::infinity();
        float minimum = numeric_limits<float>::infinity();
    } stats;

    // Parallel stats computation: every task computes its own stats, which are combined at the end.
    {
        vector<Stats> perLineStats(regionSize.y());

        co_await ThreadPool::global().parallelForAsync<int>(
            region.min.y(),
            region.max.y(),
            numSamples,
            [&](int y) {
                Stats& lineStats = perLineStats[y - region.min.y()];

                for (int x = region.min.x(); x < region.max.x(); ++x) {
                    for (size_t c = 0; c < nColorChannels; ++c) {
                        const auto& channel = flattened[c];
                        auto v = channel.at({x, y});
                        if (!isfinite(v)) {
                            continue;
                        }

                        lineStats.mean += v;
                        lineStats.maximum = std::max(lineStats.maximum, v);
                        lineStats.minimum = std::min(lineStats.minimum, v);
                    }
                }
            },
            priority
        );

        for (const auto& lineStats : perLineStats) {
            stats.mean += lineStats.mean;
            stats.maximum = std::max(stats.maximum, lineStats.maximum);
            stats.minimum = std::min(stats.minimum, lineStats.minimum);
        }
    }

    result->mean = numSamples > 0 ? (float)(stats.mean / numSamples) : 0;
    result->maximum = stats.maximum;
    result->minimum = stats.minimum;

    // The more pixels we have, the finer we can make the histogram without becoming noisy
    // const size_t numBins = clamp(numPixels / 512, (size_t)16, (size_t)512);
    const size_t numBins = 400;
    result->histogram.resize(numBins * nColorChannels);

    // We're going to draw our histogram in log space.
    static const float addition = 0.001f;
    static const float smallest = log(addition);

    const auto symmetricLog = [](const float val) { return val > 0 ? (log(val + addition) - smallest) : -(log(-val + addition) - smallest); };
    const auto symmetricLogInverse = [](const float val) {
        return val > 0 ? (exp(val + smallest) - addition) : -(exp(-val + smallest) - addition);
    };

    const float minLog = symmetricLog(stats.minimum);
    const float diffLog = symmetricLog(stats.maximum) - minLog;

    const auto valToBin = [&](const float val) {
        return clamp((int)(numBins * (symmetricLog(val) - minLog) / diffLog), 0, (int)numBins - 1);
    };

    result->histogramZero = valToBin(0);

    const auto binToVal = [&](const float val) { return symmetricLogInverse((diffLog * val / numBins) + minLog); };

    // In the strange case that we have 0 channels, early return, because the histogram makes no sense.
    if (nColorChannels == 0) {
        co_return result;
    }

    // Parallel histogram computation: every task computes its own histogram, which are combined at the end.
    {
        const size_t approxCost = numSamples *
            8; // constant factor to represent the increased workload of log/exp and somewhat random memory writes
        const size_t numTasks = nextMultiple(ThreadPool::global().nTasks<size_t>(0, numPixels, approxCost), (size_t)nColorChannels);
        const size_t numTasksPerChannel = numTasks / nColorChannels;

        vector<float> perTaskHistograms(numBins * nColorChannels * numTasks);

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numTasks,
            approxCost,
            [&](size_t i) {
                const size_t c = i % nColorChannels;
                const size_t taskPerChannelIndex = i / nColorChannels;

                const size_t taskStart = numPixels * taskPerChannelIndex / numTasksPerChannel;
                const size_t taskEnd = numPixels * (taskPerChannelIndex + 1) / numTasksPerChannel;

                float* const histogram = perTaskHistograms.data() + numBins * i;
                const auto& channel = flattened[c];

                const auto regionSize = region.size();
                for (size_t j = taskStart; j < taskEnd; ++j) {
                    const int x = (int)(j % regionSize.x()) + region.min.x();
                    const int y = (int)(j / regionSize.x()) + region.min.y();

                    histogram[valToBin(channel.at({x, y}))] += alphaChannel ? alphaChannel->at({x, y}) : 1;
                }
            },
            priority
        );

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numBins * nColorChannels,
            numBins * nColorChannels * numTasks,
            [&](size_t i) {
                const size_t stride = numBins * nColorChannels;
                for (size_t j = 0; j < numTasks; ++j) {
                    result->histogram[i] += perTaskHistograms[i + j * stride];
                }
            },
            priority
        );
    }

    for (size_t i = 0; i < nColorChannels; ++i) {
        for (size_t j = 0; j < numBins; ++j) {
            result->histogram[j + i * numBins] /= binToVal(j + 1) - binToVal(j);
        }
    }

    // Normalize the histogram according to the n-th largest element to avoid outliers dominating the histogram display.
    // - Ensure at least one element per color channel is excluded (typically a spike at zero)
    // - Additionally, scale by number of bins to make it a percentile-like normalization.
    auto tmp = result->histogram;
    const size_t idx = tmp.size() - 1 - (1 + numBins / 128) * nColorChannels;
    nth_element(tmp.data(), tmp.data() + idx, tmp.data() + tmp.size());

    const float norm = 1.0f / (std::max(tmp[idx], 0.1f) * 1.3f);
    for (size_t i = 0; i < nColorChannels; ++i) {
        for (size_t j = 0; j < numBins; ++j) {
            result->histogram[j + i * numBins] *= norm;
        }
    }

    co_return result;
}

Vector2f ImageCanvas::pixelOffset(const Vector2i& /*size*/) const {
    // Translate by half of a pixel to avoid pixel boundaries aligning perfectly with texels. The translation only needs to happen for axes
    // with even resolution. Odd-resolution axes are implicitly shifted by half a pixel due to the centering operation. Additionally, add
    // 0.1111111 such that our final position is almost never 0 modulo our pixel ratio, which again avoids aligned pixel boundaries with
    // texels.
    // return Vector2f{
    //     size.x() % 2 == 0 ?  0.5f : 0.0f,
    //     size.y() % 2 == 0 ? -0.5f : 0.0f,
    // } + Vector2f{0.1111111f};
    return Vector2f{-0.1111111f};
}

Matrix3f ImageCanvas::transform(const Image* image) {
    if (!image) {
        return Matrix3f::scale(Vector2f{1.0f});
    }

    TEV_ASSERT(mImage, "Coordinates are relative to the currently selected image's display window. So must have an image selected.");

    // Center image, scale to pixel space, translate to desired position, then rescale to the [-1, 1] square for drawing.
    return Matrix3f::scale(Vector2f{2.0f / m_size.x(), -2.0f / m_size.y()}) * mTransform * Matrix3f::scale(Vector2f{1.0f / mPixelRatio}) *
        Matrix3f::translate(image->centerDisplayOffset(mImage->displayWindow()) + pixelOffset(image->size())) *
        Matrix3f::scale(Vector2f{image->size()}) * Matrix3f::translate(Vector2f{-0.5f});
}

Matrix3f ImageCanvas::textureToNanogui(const Image* image) {
    if (!image) {
        return Matrix3f::scale(Vector2f{1.0f});
    }

    TEV_ASSERT(mImage, "Coordinates are relative to the currently selected image's display window. So must have an image selected.");

    // Move origin to centre of image, scale pixels, apply our transform, move origin back to top-left.
    return Matrix3f::translate(0.5f * Vector2f{m_size}) * mTransform * Matrix3f::scale(Vector2f{1.0f / mPixelRatio}) *
        Matrix3f::translate(-0.5f * Vector2f{image->size()} + image->centerDisplayOffset(mImage->displayWindow()) + pixelOffset(image->size()));
}

Matrix3f ImageCanvas::displayWindowToNanogui(const Image* image) {
    if (!image) {
        return Matrix3f::scale(Vector2f{1.0f});
    }

    // Shift texture coordinates by the data coordinate offset. It's that simple.
    return textureToNanogui(image) * Matrix3f::translate(-image->dataWindow().min);
}

} // namespace tev
