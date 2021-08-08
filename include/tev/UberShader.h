// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <nanogui/shader.h>
#include <nanogui/texture.h>

#include <Eigen/Dense>

TEV_NAMESPACE_BEGIN

class UberShader {
public:
    UberShader(nanogui::RenderPass* renderPass);
    virtual ~UberShader();

    // Draws just a checkerboard.
    void draw(const Eigen::Vector2f& pixelSize, const Eigen::Vector2f& checkerSize);

    // Draws an image.
    void draw(
        const Eigen::Vector2f& pixelSize,
        const Eigen::Vector2f& checkerSize,
        nanogui::Texture* textureImage,
        const nanogui::Matrix3f& transformImage,
        float exposure,
        float offset,
        float gamma,
        bool clipToLdr,
        ETonemap tonemap
    );

    // Draws a difference between a reference and an image.
    void draw(
        const Eigen::Vector2f& pixelSize,
        const Eigen::Vector2f& checkerSize,
        nanogui::Texture* textureImage,
        const nanogui::Matrix3f& transformImage,
        nanogui::Texture* textureReference,
        const nanogui::Matrix3f& transformReference,
        float exposure,
        float offset,
        float gamma,
        bool clipToLdr,
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
        nanogui::Texture* textureImage,
        const nanogui::Matrix3f& transformImage,
        float exposure,
        float offset,
        float gamma,
        ETonemap tonemap
    );

    void bindReferenceData(
        nanogui::Texture* textureReference,
        const nanogui::Matrix3f& transformReference,
        EMetric metric
    );

    nanogui::ref<nanogui::Shader> mShader;
    nanogui::ref<nanogui::Texture> mColorMap;

    nanogui::Color mBackgroundColor = nanogui::Color(0, 0, 0, 0);
};

TEV_NAMESPACE_END
