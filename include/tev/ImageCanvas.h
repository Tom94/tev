// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/UberShader.h>
#include <tev/Image.h>
#include <tev/Lazy.h>

#include <nanogui/canvas.h>

#include <memory>

TEV_NAMESPACE_BEGIN

struct CanvasStatistics {
    float mean;
    float maximum;
    float minimum;
    Eigen::MatrixXf histogram;
    int histogramZero;
};

class ImageCanvas : public nanogui::Canvas {
public:
    ImageCanvas(nanogui::Widget* parent, float pixelRatio);

    bool scroll_event(const nanogui::Vector2i& p, const nanogui::Vector2f& rel) override;

    void draw_contents() override;

    void draw(NVGcontext *ctx) override;

    void translate(const Eigen::Vector2f& amount);
    void scale(float amount, const Eigen::Vector2f& origin);
    float extractScale() const {
        return std::sqrt(mTransform.linear().determinant());
    }

    void setExposure(float exposure) {
        mExposure = exposure;
    }

    void setOffset(float offset) {
        mOffset = offset;
    }

    void setGamma(float gamma) {
        mGamma = gamma;
    }

    float applyExposureAndOffset(float value) const;

    void setImage(std::shared_ptr<Image> image) {
        mImage = image;
    }

    void setReference(std::shared_ptr<Image> reference) {
        mReference = reference;
    }

    void setRequestedChannelGroup(const std::string& groupName) {
        mRequestedChannelGroup = groupName;
    }

    Eigen::Vector2i getImageCoords(const Image& image, Eigen::Vector2i mousePos);

    void getValuesAtNanoPos(Eigen::Vector2i nanoPos, std::vector<float>& result, const std::vector<std::string>& channels);
    std::vector<float> getValuesAtNanoPos(Eigen::Vector2i nanoPos, const std::vector<std::string>& channels) {
        std::vector<float> result;
        getValuesAtNanoPos(nanoPos, result, channels);
        return result;
    }

    ETonemap tonemap() const {
        return mTonemap;
    }

    void setTonemap(ETonemap tonemap) {
        mTonemap = tonemap;
    }

    static Eigen::Vector3f applyTonemap(const Eigen::Vector3f& value, float gamma, ETonemap tonemap);
    Eigen::Vector3f applyTonemap(const Eigen::Vector3f& value) const {
        return applyTonemap(value, mGamma, mTonemap);
    }

    EMetric metric() const {
        return mMetric;
    }

    void setMetric(EMetric metric) {
        mMetric = metric;
    }

    static float applyMetric(float value, float reference, EMetric metric);
    float applyMetric(float value, float reference) const {
        return applyMetric(value, reference, mMetric);
    }

    const nanogui::Color& backgroundColor() {
        return mShader->backgroundColor();
    }

    void setBackgroundColor(const nanogui::Color& color) {
        mShader->setBackgroundColor(color);
    }

    void fitImageToScreen(const Image& image);
    void resetTransform();

    void setClipToLdr(bool value) {
        mClipToLdr = value;
    }

    bool clipToLdr() const {
        return mClipToLdr;
    }

    std::vector<float> getHdrImageData(bool divideAlpha) const;
    std::vector<char> getLdrImageData(bool divideAlpha) const;

    void saveImage(const filesystem::path& filename) const;

    std::shared_ptr<Lazy<std::shared_ptr<CanvasStatistics>>> canvasStatistics();

    static nanogui::Matrix3f toNanogui(const Eigen::Matrix3f& transform) {
        nanogui::Matrix3f result;
        for (int m = 0; m < 3; ++m) {
            for (int n = 0; n < 3; ++n) {
                result.m[n][m] = transform(m, n);
            }
        }
        return result;
    }

private:
    static std::vector<Channel> channelsFromImages(
        std::shared_ptr<Image> image,
        std::shared_ptr<Image> reference,
        const std::string& requestedChannelGroup,
        EMetric metric
    );

    static std::shared_ptr<CanvasStatistics> computeCanvasStatistics(
        std::shared_ptr<Image> image,
        std::shared_ptr<Image> reference,
        const std::string& requestedChannelGroup,
        EMetric metric
    );

    Eigen::Vector2f pixelOffset(const Eigen::Vector2i& size) const;

    // Assembles the transform from canonical space to
    // the [-1, 1] square for the current image.
    Eigen::Transform<float, 2, 2> transform(const Image* image);
    Eigen::Transform<float, 2, 2> textureToNanogui(const Image* image);

    float mPixelRatio = 1;
    float mExposure = 0;
    float mOffset = 0;
    float mGamma = 2.2f;

    bool mClipToLdr = false;

    std::shared_ptr<Image> mImage;
    std::shared_ptr<Image> mReference;

    std::string mRequestedChannelGroup = "";

    Eigen::Transform<float, 2, 2> mTransform = Eigen::Affine2f::Identity();

    std::unique_ptr<UberShader> mShader;

    ETonemap mTonemap = SRGB;
    EMetric mMetric = Error;

    std::map<std::string, std::shared_ptr<Lazy<std::shared_ptr<CanvasStatistics>>>> mMeanValues;
    // A custom threadpool is used to ensure progress
    // on the global threadpool, even when excessively
    // many mean value computations are scheduled.
    ThreadPool mMeanValueThreadPool;
};

TEV_NAMESPACE_END
