// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/GlTexture.h"

#include <nanogui/glutil.h>

TEV_NAMESPACE_BEGIN

class UberShader {
public:
    UberShader();
    virtual ~UberShader();

    // Draws a difference between a reference and an image.
    void draw(
        const GlTexture* textureImage,
        const Eigen::Matrix3f& transformImage,
        const GlTexture* textureReference,
        const Eigen::Matrix3f& transformReference,
        float exposure,
        float offset,
        ETonemap tonemap,
        EMetric metric
    );

    // Draws an image.
    void draw(
        const GlTexture* textureImage,
        const Eigen::Matrix3f& transformImage,
        float exposure,
        float offset,
        ETonemap tonemap
    );


private:
    void bindImageData(
        const GlTexture* textureImage,
        const Eigen::Matrix3f& transformImage,
        float exposure,
        float offset,
        ETonemap tonemap
    );

    void bindReferenceData(
        const GlTexture* textureReference,
        const Eigen::Matrix3f& transformReference,
        EMetric metric
    );

    nanogui::GLShader mShader;
    GlTexture mColorMap;
};

TEV_NAMESPACE_END
