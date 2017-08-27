// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

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
        std::array<const GlTexture*, 4> texturesImage,
        const Eigen::Matrix3f& transformImage,
        std::array<const GlTexture*, 4> texturesReference,
        const Eigen::Matrix3f& transformReference,
        float exposure,
        ETonemap tonemap,
        EMetric metric
    );

    // Draws an image.
    void draw(
        std::array<const GlTexture*, 4> texturesImage,
        const Eigen::Matrix3f& transformImage,
        float exposure,
        ETonemap tonemap
    );


private:
    nanogui::GLShader mShader;
};

TEV_NAMESPACE_END
