// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/ImageCanvas.h"

#include <nanogui/theme.h>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageCanvas::ImageCanvas(nanogui::Widget* parent, float pixelRatio)
: GLCanvas(parent), mPixelRatio(pixelRatio) {
    mTextureBlack.setData({ 0.0 }, Vector2i::Constant(1), 1);
    mTextureWhite.setData({ 1.0 }, Vector2i::Constant(1), 1);
    setDrawBorder(false);
}

bool ImageCanvas::mouseMotionEvent(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    if (GLCanvas::mouseMotionEvent(p, rel, button, modifiers)) {
        return true;
    }

    // If left mouse button is held
    if ((button & 1) != 0) {
        mTransform = Translation2f(rel.cast<float>()) * mTransform;
    }

    return false;
}

bool ImageCanvas::scrollEvent(const Vector2i& p, const Vector2f& rel) {
    if (GLCanvas::scrollEvent(p, rel)) {
        return true;
    }

    float scaleFactor = pow(1.1f, rel.y());

    // Use the current cursor position as the origin to scale around.
    Vector2f offset = -(p - position()).cast<float>() + 0.5f * mSize.cast<float>();
    auto scaleTransform =
        Translation2f(-offset) *
        Scaling(scaleFactor) *
        Translation2f(offset);

    mTransform = scaleTransform * mTransform;

    return true;
}

void ImageCanvas::drawGL() {
    mCheckerboardShader.draw(
        2.0f * mSize.cast<float>().cwiseInverse() / mPixelRatio,
        Vector2f::Constant(20)
    );

    if (!mImage) {
        return;
    }

    auto getTextures = [this](Image& image) {
        const auto& imageChannels = getChannels(image);
        return array<const GlTexture*, 4>{{
            imageChannels.size() > 0 ? image.texture(imageChannels[0]) : &mTextureBlack,
            imageChannels.size() > 1 ? image.texture(imageChannels[1]) : &mTextureBlack,
            imageChannels.size() > 2 ? image.texture(imageChannels[2]) : &mTextureBlack,
            imageChannels.size() > 3 ? image.texture(imageChannels[3]) : &mTextureWhite,
        }};
    };

    if (mReference) {
        mShader.draw(
            getTextures(*mImage),
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(mImage.get()).inverse().matrix(),
            getTextures(*mReference),
            transform(mReference.get()).inverse().matrix(),
            mExposure,
            mOffset,
            mTonemap,
            mMetric
        );
    } else {
        mShader.draw(
            getTextures(*mImage),
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(mImage.get()).inverse().matrix(),
            mExposure,
            mOffset,
            mTonemap
        );
    }
}

void ImageCanvas::draw(NVGcontext *ctx) {
    GLCanvas::draw(ctx);

    if (mImage) {
        auto texToNano = textureToNanogui(mImage.get());
        auto nanoToTex = texToNano.inverse();

        Vector2f pixelSize = texToNano * Vector2f::Ones() - texToNano * Vector2f::Zero();

        Vector2f topLeft = (nanoToTex * Vector2f::Zero());
        Vector2f bottomRight = (nanoToTex * mSize.cast<float>());

        Vector2i startIndices = Vector2i{
            static_cast<int>(floor(topLeft.x())),
            static_cast<int>(floor(topLeft.y())),
        };

        Vector2i endIndices = Vector2i{
            static_cast<int>(ceil(bottomRight.x())),
            static_cast<int>(ceil(bottomRight.y())),
        };

        if (pixelSize.x() > 50) {
            float fontSize = pixelSize.x() / 6;
            float fontAlpha = min(1.0f, (pixelSize.x() - 50) / 30);

            vector<string> channels = getChannels(*mImage);
            // Remove duplicates
            channels.erase(unique(begin(channels), end(channels)), end(channels));

            vector<Color> colors;
            for (const auto& channel : channels) {
                colors.emplace_back(Channel::color(channel));
            }

            nvgFontSize(ctx, fontSize);
            nvgFontFace(ctx, "sans");
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

            Vector2i cur;
            vector<float> values;
            for (cur.y() = startIndices.y(); cur.y() < endIndices.y(); ++cur.y()) {
                for (cur.x() = startIndices.x(); cur.x() < endIndices.x(); ++cur.x()) {
                    Vector2i nano = (texToNano * (cur.cast<float>() + Vector2f::Constant(0.5f))).cast<int>();
                    getValues(nano, values);

                    TEV_ASSERT(values.size() >= colors.size(), "Can not have more values than channels.");

                    for (size_t i = 0; i < colors.size(); ++i) {
                        string str = tfm::format("%.4f", values[i]);
                        Vector2f pos{
                            mPos.x() + nano.x(),
                            mPos.y() + nano.y() + (i - 0.5f * (values.size() - 1)) * fontSize,
                        };

                        // First draw a shadow such that the font will be visible on white background.
                        nvgFontBlur(ctx, 2);
                        nvgFillColor(ctx, Color(0.0f, fontAlpha));
                        nvgText(ctx, pos.x() + 1, pos.y() + 1, str.c_str(), nullptr);

                        // Actual text.
                        nvgFontBlur(ctx, 0);
                        Color col = colors[i];
                        nvgFillColor(ctx, Color(col.r(), col.g(), col.b(), fontAlpha));
                        nvgText(ctx, pos.x(), pos.y(), str.c_str(), nullptr);
                    }
                }
            }
        }
    }

    // If we're not in fullscreen mode...
    if (mPos.x() != 0) {
        // Draw an inner drop shadow. (adapted from nanogui::Window)
        int ds = mTheme->mWindowDropShadowSize, cr = mTheme->mWindowCornerRadius;
        NVGpaint shadowPaint = nvgBoxGradient(
            ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y(), cr * 2, ds * 2,
            mTheme->mTransparent, mTheme->mDropShadow
        );

        nvgSave(ctx);
        nvgResetScissor(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
        nvgRoundedRect(ctx, mPos.x() + ds, mPos.y() + ds, mSize.x() - 2 * ds, mSize.y() - 2 * ds, cr);
        nvgPathWinding(ctx, NVG_HOLE);
        nvgFillPaint(ctx, shadowPaint);
        nvgFill(ctx);
        nvgRestore(ctx);
    }
}

vector<string> ImageCanvas::getChannels(const Image& image) {
    vector<vector<string>> groups = {
        { "R", "G", "B" },
        { "r", "g", "b" },
        { "X", "Y", "Z" },
        { "x", "y", "z" },
        { "U", "V" },
        { "u", "v" },
        { "Z" },
        { "z" },
    };

    string layerPrefix = mRequestedLayer.empty() ? "" : (mRequestedLayer + ".");

    vector<string> result;
    for (const auto& group : groups) {
        for (size_t i = 0; i < group.size(); ++i) {
            const auto& name = layerPrefix + group[i];
            if (image.hasChannel(name)) {
                result.emplace_back(name);
            }
        }

        if (!result.empty()) {
            break;
        }
    }

    string alphaChannelName = layerPrefix + "A";

    // No channels match the given groups; fall back to the first 3 channels.
    if (result.empty()) {
        const auto& channelNames = image.channelsInLayer(mRequestedLayer);
        for (const auto& name : channelNames) {
            if (name != alphaChannelName) {
                result.emplace_back(name);
            }

            if (result.size() >= 3) {
                break;
            }
        }
    }

    // If we found just 1 channel, let's display is as grayscale
    // by duplicating it twice.
    if (result.size() == 1) {
        result.push_back(result[0]);
        result.push_back(result[0]);
    }

    // If there is an alpha layer, use it
    if (image.hasChannel(alphaChannelName)) {
        result.emplace_back(alphaChannelName);
    }

    return result;
}

Vector2i ImageCanvas::getImageCoords(const Image& image, Vector2i mousePos) {
    Vector2f imagePos = textureToNanogui(&image).inverse() * mousePos.cast<float>();
    return {
        static_cast<int>(floor(imagePos.x())),
        static_cast<int>(floor(imagePos.y())),
    };
}

float ImageCanvas::applyMetric(float diff, float reference) {
    switch (mMetric) {
        case EMetric::Error:                 return diff;
        case EMetric::AbsoluteError:         return abs(diff);
        case EMetric::SquaredError:          return diff * diff;
        case EMetric::RelativeAbsoluteError: return abs(diff) / (reference + 0.01f);
        case EMetric::RelativeSquaredError:  return diff * diff / (reference * reference + 0.0001f);
        default:
            throw runtime_error{"Invalid metric selected."};
    }
}

void ImageCanvas::getValues(Vector2i mousePos, vector<float>& result) {
    result.clear();
    if (!mImage) {
        return;
    }

    Vector2i imageCoords = getImageCoords(*mImage, mousePos);
    const auto& channels = getChannels(*mImage);

    for (const auto& channel : channels) {
        result.push_back(mImage->channel(channel)->eval(imageCoords));
    }

    // Subtract reference if it exists.
    if (mReference) {
        Vector2i referenceCoords = getImageCoords(*mReference, mousePos);
        const auto& referenceChannels = getChannels(*mReference);
        for (size_t i = 0; i < result.size(); ++i) {
            float reference = i < referenceChannels.size() ?
                mReference->channel(referenceChannels[i])->eval(referenceCoords) :
                0.0f;

            result[i] = applyMetric(result[i] - reference, reference);
        }
    }
}

void ImageCanvas::fitImageToScreen(const Image& image) {
    Vector2f nanoguiImageSize = image.size().cast<float>() / mPixelRatio;
    mTransform = Scaling(mSize.cast<float>().cwiseQuotient(nanoguiImageSize).minCoeff());
}

void ImageCanvas::resetTransform() {
    mTransform = Affine2f::Identity();
}

Transform<float, 2, 2> ImageCanvas::transform(const Image* image) {
    if (!image) {
        return Transform<float, 2, 0>::Identity();
    }

    // Center image, scale to pixel space, translate to desired position,
    // then rescale to the [-1, 1] square for drawing.
    return
        Scaling(2.0f / mSize.x(), -2.0f / mSize.y()) *
        mTransform *
        // Translate by 1/10000th of a pixel to avoid pixel edges lying exactly on fragment edges.
        // This avoids artifacts caused by inconsistent rounding.
        Translation2f(Vector2f::Constant(0.0001f)) *
        Scaling(image->size().cast<float>() / mPixelRatio) *
        Translation2f(Vector2f::Constant(-0.5f));
}

Transform<float, 2, 2> ImageCanvas::textureToNanogui(const Image* image) {
    if (!image) {
        return Transform<float, 2, 0>::Identity();
    }

    // Move origin to centre of image, scale pixels, apply our transform, move origin back to top-left.
    return
        Translation2f(0.5f * mSize.cast<float>()) *
        mTransform *
        Scaling(1.0f / mPixelRatio) *
        Translation2f(-0.5f * image->size().cast<float>());
}

TEV_NAMESPACE_END
