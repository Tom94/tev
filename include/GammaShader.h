// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include "../include/GlTexture.h"

#include <nanogui/glutil.h>

class GammaShader {
public:
    GammaShader();
    virtual ~GammaShader();

    // Draws an image using each supplied texture as the R, G, B, and then A channel.
    void draw(std::array<const GlTexture*, 4> textures, float exposure, const Eigen::Matrix3f& transform);


private:
    nanogui::GLShader mShader;
};
