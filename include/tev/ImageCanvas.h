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

#pragma once

#include <tev/Box.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/Lazy.h>
#include <tev/UberShader.h>

#include <nanogui/canvas.h>

#include <memory>
#include <optional>

namespace tev {

struct CanvasStatistics {
    float mean;
    float maximum;
    float minimum;
    std::vector<float> histogram;
    std::vector<nanogui::Color> histogramColors;
    int nChannels;
    int histogramZero;
};

class ImageCanvas : public nanogui::Canvas {
public:
    ImageCanvas(nanogui::Widget* parent);

    bool scroll_event(const nanogui::Vector2i& p, const nanogui::Vector2f& rel) override;

    void draw_contents() override;

    void draw(NVGcontext* ctx) override;

    void translate(const nanogui::Vector2f& amount);
    void scale(float amount, const nanogui::Vector2f& origin);
    float scale() const { return nanogui::extractScale(mTransform); }

    void setExposure(float exposure) { mExposure = exposure; }
    void setOffset(float offset) { mOffset = offset; }
    void setGamma(float gamma) { mGamma = gamma; }

    float applyExposureAndOffset(float value) const;

    void setImage(std::shared_ptr<Image> image) { mImage = image; }
    void setReference(std::shared_ptr<Image> reference) { mReference = reference; }
    void setRequestedChannelGroup(std::string_view groupName) { mRequestedChannelGroup = groupName; }

    nanogui::Vector2i getImageCoords(const Image* image, nanogui::Vector2i mousePos);
    nanogui::Vector2i getDisplayWindowCoords(const Image* image, nanogui::Vector2i mousePos);

    void getValuesAtNanoPos(nanogui::Vector2i nanoPos, std::vector<float>& result, std::span<const std::string> channels);
    std::vector<float> getValuesAtNanoPos(nanogui::Vector2i nanoPos, std::span<const std::string> channels) {
        std::vector<float> result;
        getValuesAtNanoPos(nanoPos, result, channels);
        return result;
    }

    ETonemap tonemap() const { return mTonemap; }
    void setTonemap(ETonemap tonemap) { mTonemap = tonemap; }

    static nanogui::Vector3f applyTonemap(const nanogui::Vector3f& value, float gamma, ETonemap tonemap);
    nanogui::Vector3f applyTonemap(const nanogui::Vector3f& value) const { return applyTonemap(value, mGamma, mTonemap); }

    EMetric metric() const { return mMetric; }
    void setMetric(EMetric metric) { mMetric = metric; }

    static float applyMetric(float value, float reference, EMetric metric);
    float applyMetric(float value, float reference) const { return applyMetric(value, reference, mMetric); }

    std::optional<Box2i> crop() { return mCrop; }
    void setCrop(const std::optional<Box2i>& crop) { mCrop = crop; }
    Box2i cropInImageCoords() const;

    auto backgroundColor() { return mShader->backgroundColor(); }
    void setBackgroundColor(const nanogui::Color& color) { mShader->setBackgroundColor(color); }

    void fitImageToScreen(const Image& image);
    void resetTransform();

    bool clipToLdr() const { return mClipToLdr; }
    void setClipToLdr(bool value) { mClipToLdr = value; }

    EInterpolationMode minFilter() const { return mMinFilter; }
    void setMinFilter(EInterpolationMode value) { mMinFilter = value; }

    EInterpolationMode magFilter() const { return mMagFilter; }
    void setMagFilter(EInterpolationMode value) { mMagFilter = value; }

    // The following functions return four values per pixel in RGBA order. The number of pixels is given by `imageDataSize()`. If the canvas
    // does not currently hold an image, or no channels are displayed, then zero pixels are returned.
    nanogui::Vector2i imageDataSize() const { return cropInImageCoords().size(); }
    std::vector<float> getHdrImageData(bool divideAlpha, int priority) const;
    std::vector<char> getLdrImageData(bool divideAlpha, int priority) const;

    void saveImage(const fs::path& filename) const;

    std::shared_ptr<Lazy<std::shared_ptr<CanvasStatistics>>> canvasStatistics();

    void purgeCanvasStatistics(int imageId);

    float pixelRatio() const { return mPixelRatio; }
    void setPixelRatio(float ratio) { mPixelRatio = ratio; }

private:
    static std::vector<Channel> channelsFromImages(
        std::shared_ptr<Image> image, std::shared_ptr<Image> reference, std::string_view requestedChannelGroup, EMetric metric, int priority
    );

    static Task<std::shared_ptr<CanvasStatistics>> computeCanvasStatistics(
        std::shared_ptr<Image> image,
        std::shared_ptr<Image> reference,
        std::string_view requestedChannelGroup,
        EMetric metric,
        const Box2i& region,
        int priority
    );

    void drawPixelValuesAsText(NVGcontext* ctx);
    void drawCoordinateSystem(NVGcontext* ctx);
    void drawEdgeShadows(NVGcontext* ctx);

    nanogui::Vector2f pixelOffset(const nanogui::Vector2i& size) const;

    // Assembles the transform from canonical space to the [-1, 1] square for the current image.
    nanogui::Matrix3f transform(const Image* image);
    nanogui::Matrix3f textureToNanogui(const Image* image);
    nanogui::Matrix3f displayWindowToNanogui(const Image* image);

    float mPixelRatio = 1;
    float mExposure = 0;
    float mOffset = 0;
    float mGamma = 2.2f;

    bool mClipToLdr = false;

    EInterpolationMode mMinFilter = EInterpolationMode::Trilinear;
    EInterpolationMode mMagFilter = EInterpolationMode::Nearest;

    std::shared_ptr<Image> mImage;
    std::shared_ptr<Image> mReference;

    std::string mRequestedChannelGroup = "";

    nanogui::Matrix3f mTransform = nanogui::Matrix3f::scale(nanogui::Vector3f(1.0f));

    std::unique_ptr<UberShader> mShader;

    ETonemap mTonemap = SRGB;
    EMetric mMetric = Error;
    std::optional<Box2i> mCrop;

    std::map<std::string, std::shared_ptr<Lazy<std::shared_ptr<CanvasStatistics>>>> mCanvasStatistics;
    std::map<int, std::vector<std::string>> mImageIdToCanvasStatisticsKey;
};

} // namespace tev
