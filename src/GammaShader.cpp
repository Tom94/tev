// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/GammaShader.h"

using namespace Eigen;
using namespace nanogui;

GammaShader::GammaShader() {

    mShader.init(
        "tonemapper",

        // Vertex shader
        R"(#version 330
        uniform mat3 modelViewProj;
        in vec2 position;
        out vec2 uv;

        void main() {
            uv = position;
            gl_Position = vec4(modelViewProj * vec3(position, 1.0), 1.0);
        })",

        // Fragment shader
        R"(#version 330
        uniform sampler2D imageRed;
        uniform sampler2D imageGreen;
        uniform sampler2D imageBlue;
        uniform sampler2D imageAlpha;

        uniform float exposure;
        out vec4 color;
        in vec2 uv;

        vec3 tonemap(vec3 col) {
            return pow(2.0, exposure) * pow(col, vec3(1.0 / 2.2));
        }

        void main() {
            vec4 imageVal = vec4(
                texture(imageRed, uv).x,
                texture(imageGreen, uv).x,
                texture(imageBlue, uv).x,
                texture(imageAlpha, uv).x
            );
            color = vec4(tonemap(imageVal.xyz), imageVal.a);
        })"
    );

    // 2 Triangles
    MatrixXu indices(3, 2);
    indices.col(0) << 0, 1, 2;
    indices.col(1) << 2, 3, 0;

    MatrixXf positions(2, 4);
    positions.col(0) << 0, 0;
    positions.col(1) << 1, 0;
    positions.col(2) << 1, 1;
    positions.col(3) << 0, 1;

    mShader.bind();
    mShader.uploadIndices(indices);
    mShader.uploadAttrib("position", positions);
}

GammaShader::~GammaShader() {
    mShader.free();
}

void GammaShader::draw(std::array<const GlTexture*, 4> textures, float exposure, const Matrix3f& transform) {
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]->id());
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mShader.bind();
    mShader.setUniform("imageRed", 0);
    mShader.setUniform("imageGreen", 1);
    mShader.setUniform("imageBlue", 2);
    mShader.setUniform("imageAlpha", 3);

    mShader.setUniform("modelViewProj", transform);
    mShader.setUniform("exposure", exposure);
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}
