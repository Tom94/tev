// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/UberShader.h"
#include "../include/Image.h"

#include <nanogui/glcanvas.h>

#include <memory>

TEV_NAMESPACE_BEGIN

class ImageCanvas : public nanogui::GLCanvas {
public:
    ImageCanvas(nanogui::Widget* parent, float pixelRatio);

    bool mouseMotionEvent(const Eigen::Vector2i& p, const Eigen::Vector2i& rel, int button, int modifiers) override;

    bool scrollEvent(const Eigen::Vector2i& p, const Eigen::Vector2f& rel) override;

    void drawGL() override;

    void draw(NVGcontext *ctx) override;

    void setExposure(float exposure) {
        mExposure = exposure;
    }

    void setOffset(float offset) {
        mOffset = offset;
    }

    void setImage(std::shared_ptr<Image> image) {
        mImage = image;
    }

    void setReference(std::shared_ptr<Image> reference) {
        mReference = reference;
    }

    void setRequestedLayer(const std::string& layerName) {
        mRequestedLayer = layerName;
    }

    std::vector<std::string> getChannels(const Image& image);

    Eigen::Vector2i getImageCoords(const Image& image, Eigen::Vector2i mousePos);

    float applyMetric(float value, float reference);

    void getValues(Eigen::Vector2i mousePos, std::vector<float>& result);
    std::vector<float> getValues(Eigen::Vector2i mousePos) {
        std::vector<float> result;
        getValues(mousePos, result);
        return result;
    }

    ETonemap tonemap() {
        return mTonemap;
    }

    void setTonemap(ETonemap tonemap) {
        mTonemap = tonemap;
    }

    EMetric metric() {
        return mMetric;
    }

    void setMetric(EMetric metric) {
        mMetric = metric;
    }

    void fitImageToScreen(const Image& image);
    void resetTransform();

private:
    void translate(const Eigen::Vector2f& amount);
    void scale(float amount, const Eigen::Vector2f& origin);

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
};

TEV_NAMESPACE_END
