// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/GlTexture.h>

#include <nanogui/glutil.h>

TEV_NAMESPACE_BEGIN

class UberShader {
public:
    UberShader();
    virtual ~UberShader();

    // Draws just a checkerboard.
    void draw(const Eigen::Vector2f& pixelSize, const Eigen::Vector2f& checkerSize);

    // Draws an image.
    void draw(
        const Eigen::Vector2f& pixelSize,
        const Eigen::Vector2f& checkerSize,
        const GlTexture* textureImage,
        const Eigen::Matrix3f& transformImage,
        float exposure,
        float offset,
        float gamma,
        ETonemap tonemap
    );

    // Draws a difference between a reference and an image.
    void draw(
        const Eigen::Vector2f& pixelSize,
        const Eigen::Vector2f& checkerSize,
        const GlTexture* textureImage,
        const Eigen::Matrix3f& transformImage,
        const GlTexture* textureReference,
        const Eigen::Matrix3f& transformReference,
        float exposure,
        float offset,
        float gamma,
        ETonemap tonemap,
        EMetric metric
    );

    const nanogui::Color& backgroundColor() {
        return mBackgroundColor;
    }

    void setBackgroundColor(const nanogui::Color& color) {
        mBackgroundColor = color;
    }

private:
    void bindCheckerboardData(const Eigen::Vector2f& pixelSize, const Eigen::Vector2f& checkerSize);

    void bindImageData(
        const GlTexture* textureImage,
        const Eigen::Matrix3f& transformImage,
        float exposure,
        float offset,
        float gamma,
        ETonemap tonemap
    );

    void bindReferenceData(
        const GlTexture* textureReference,
        const Eigen::Matrix3f& transformReference,
        EMetric metric
    );

    nanogui::GLShader mShader;
    GlTexture mColorMap;

    nanogui::Color mBackgroundColor = nanogui::Color(0, 0, 0, 0);
};

TEV_NAMESPACE_END
