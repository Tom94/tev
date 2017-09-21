// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/UberShader.h>
#include <tev/Image.h>
#include <tev/Lazy.h>

#include <nanogui/glcanvas.h>

#include <memory>

TEV_NAMESPACE_BEGIN

class ImageCanvas : public nanogui::GLCanvas {
public:
    ImageCanvas(nanogui::Widget* parent, float pixelRatio);

    bool scrollEvent(const Eigen::Vector2i& p, const Eigen::Vector2f& rel) override;

    void drawGL() override;

    void draw(NVGcontext *ctx) override;

    void translate(const Eigen::Vector2f& amount);
    void scale(float amount, const Eigen::Vector2f& origin);

    void setExposure(float exposure) {
        mExposure = exposure;
    }

    void setOffset(float offset) {
        mOffset = offset;
    }

    float applyExposureAndOffset(float value);

    void setImage(std::shared_ptr<Image> image) {
        mImage = image;
    }

    void setReference(std::shared_ptr<Image> reference) {
        mReference = reference;
    }

    void setRequestedLayer(const std::string& layerName) {
        mRequestedLayer = layerName;
    }

    std::vector<std::string> getChannels(const Image& image) const;

    Eigen::Vector2i getImageCoords(const Image& image, Eigen::Vector2i mousePos);

    void getValuesAtNanoPos(Eigen::Vector2i nanoPos, std::vector<float>& result);
    std::vector<float> getValuesAtNanoPos(Eigen::Vector2i mousePos) {
        std::vector<float> result;
        getValuesAtNanoPos(mousePos, result);
        return result;
    }

    ETonemap tonemap() const {
        return mTonemap;
    }

    void setTonemap(ETonemap tonemap) {
        mTonemap = tonemap;
    }

    Eigen::Vector3f applyTonemap(const Eigen::Vector3f& value) const;

    EMetric metric() const {
        return mMetric;
    }

    void setMetric(EMetric metric) {
        mMetric = metric;
    }

    float applyMetric(float value, float reference) const;

    void fitImageToScreen(const Image& image);
    void resetTransform();

    void saveImage(const std::string& filename);

    std::shared_ptr<Lazy<float>> meanValue();

private:
    std::vector<Channel> channelsFromDisplayedImage() const;
    float computeMeanValue() const;

    Eigen::Vector2f pixelOffset(const Eigen::Vector2i& size) const;

    // Assembles the transform from canonical space to
    // the [-1, 1] square for the current image.
    Eigen::Transform<float, 2, 2> transform(const Image* image);
    Eigen::Transform<float, 2, 2> textureToNanogui(const Image* image);

    float mPixelRatio = 1;
    float mExposure = 0;
    float mOffset = 0;
    std::shared_ptr<Image> mImage;
    std::shared_ptr<Image> mReference;

    std::string mRequestedLayer;

    Eigen::Transform<float, 2, 2> mTransform = Eigen::Affine2f::Identity();

    UberShader mShader;

    ETonemap mTonemap = SRGB;
    EMetric mMetric = Error;

    std::map<std::string, std::shared_ptr<Lazy<float>>> mMeanValues;
    ThreadPool mMeanValueThreadPool;
};

TEV_NAMESPACE_END
