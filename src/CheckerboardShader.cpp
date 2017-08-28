// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/CheckerboardShader.h"

using namespace Eigen;
using namespace nanogui;

TEV_NAMESPACE_BEGIN

CheckerboardShader::CheckerboardShader() {
    mShader.init(
        "tonemapper",

        // Vertex shader
        R"(#version 330
        uniform vec2 pixelSize;
        uniform vec2 checkerSize;
        in vec2 position;
        out vec2 uv;

        void main() {
            uv = position / (pixelSize * checkerSize);
            gl_Position = vec4(position, 1.0, 1.0);
        })",

        // Fragment shader
        R"(#version 330
        out vec4 color;
        in vec2 uv;

        void main() {
            vec3 darkGray = vec3(0.5, 0.5, 0.5);
            vec3 lightGray = vec3(0.6, 0.6, 0.6);

            vec2 flooredUv = vec2(floor(uv.x), floor(uv.y));

            vec3 gray = mod(int(floor(uv.x) + floor(uv.y)), 2) == 0 ? darkGray : lightGray;
            color = vec4(gray, 1.0);
        })"
    );

    // 2 Triangles
    MatrixXu indices(3, 2);
    indices.col(0) << 0, 1, 2;
    indices.col(1) << 2, 3, 0;

    MatrixXf positions(2, 4);
    positions.col(0) << -1, -1;
    positions.col(1) <<  1, -1;
    positions.col(2) <<  1,  1;
    positions.col(3) << -1,  1;

    mShader.bind();
    mShader.uploadIndices(indices);
    mShader.uploadAttrib("position", positions);
}

CheckerboardShader::~CheckerboardShader() {
    mShader.free();
}

void CheckerboardShader::draw(const Eigen::Vector2f& pixelSize, const Eigen::Vector2f& checkerSize) {
    mShader.bind();
    mShader.setUniform("pixelSize", pixelSize);
    mShader.setUniform("checkerSize", checkerSize);
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

TEV_NAMESPACE_END
