// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include <nanogui/glutil.h>

class CheckerboardShader {
public:
    CheckerboardShader();
    virtual ~CheckerboardShader();

    void draw(const Eigen::Vector2f& pixelSize, const Eigen::Vector2f& checkerSize);

private:
    nanogui::GLShader mShader;
};
