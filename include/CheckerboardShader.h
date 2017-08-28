// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/Common.h"

#include <nanogui/glutil.h>

TEV_NAMESPACE_BEGIN

class CheckerboardShader {
public:
    CheckerboardShader();
    virtual ~CheckerboardShader();

    void draw(const Eigen::Vector2f& pixelSize, const Eigen::Vector2f& checkerSize);

private:
    nanogui::GLShader mShader;
};

TEV_NAMESPACE_END
