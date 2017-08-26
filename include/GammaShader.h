// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include "../include/GlTexture.h"

#include <nanogui/glutil.h>

#include <array>

class GammaShader {
public:
    GammaShader();
    virtual ~GammaShader();

    /// Draw an RGB image.
    void draw(std::array<const GlTexture*, 3> textures, float exposure, const Eigen::Matrix3f& transform);

    /// Draw a grayscale image.
    void draw(const GlTexture* texture, float exposure, const Eigen::Matrix3f& transform) {
        draw({{texture, texture, texture}}, exposure, transform);
    }

private:
    nanogui::GLShader mShader;
};
