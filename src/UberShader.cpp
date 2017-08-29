// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/UberShader.h"

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

UberShader::UberShader() {
    mShader.define("SRGB",        to_string(ETonemap::SRGB));
    mShader.define("GAMMA",       to_string(ETonemap::Gamma));
    mShader.define("FALSE_COLOR", to_string(ETonemap::FalseColor));
    mShader.define("POS_NEG",     to_string(ETonemap::PositiveNegative));

    mShader.define("ERROR",                   to_string(EMetric::Error));
    mShader.define("ABSOLUTE_ERROR",          to_string(EMetric::AbsoluteError));
    mShader.define("SQUARED_ERROR",           to_string(EMetric::SquaredError));
    mShader.define("RELATIVE_ABSOLUTE_ERROR", to_string(EMetric::RelativeAbsoluteError));
    mShader.define("RELATIVE_SQUARED_ERROR",  to_string(EMetric::RelativeSquaredError));

    mShader.init(
        "ubershader",

        // Vertex shader
        R"(#version 330
        uniform mat3 imageTransform;
        uniform mat3 referenceTransform;

        in vec2 position;

        out vec2 imageUv;
        out vec2 referenceUv;

        void main() {
            imageUv = (imageTransform * vec3(position, 1.0)).xy;
            referenceUv = (referenceTransform * vec3(position, 1.0)).xy;

            gl_Position = vec4(position, 1.0, 1.0);
        })",

        // Fragment shader
        R"(#version 330

        uniform sampler2D imageRed;
        uniform sampler2D imageGreen;
        uniform sampler2D imageBlue;
        uniform sampler2D imageAlpha;

        uniform sampler2D referenceRed;
        uniform sampler2D referenceGreen;
        uniform sampler2D referenceBlue;
        uniform sampler2D referenceAlpha;
        uniform bool hasReference;

        uniform float exposure;
        uniform float offset;
        uniform int tonemap;
        uniform int metric;

        in vec2 imageUv;
        in vec2 referenceUv;

        out vec4 color;

        float average(vec3 col) {
            return (col.r + col.g + col.b) / 3.0;
        }

        vec3 applyExposureAndOffset(vec3 col) {
            return pow(2.0, exposure) * col + offset;
        }

        vec3 falseColor(float v) {
            vec3 c = vec3(1.0);
            v = clamp(v, 0.0, 1.0);

            if (v < 0.25) {
                c.r = 0.0;
                c.g = 4.0 * v;
            } else if (v < 0.5) {
                c.r = 0.0;
                c.b = 1.0 + 4.0 * (0.25 - v);
            } else if (v < 0.75) {
                c.r = 4.0 * (v - 0.5);
                c.b = 0.0;
            } else {
                c.g = 1.0 + 4.0 * (0.75 - v);
                c.b = 0.0;
            }

            return c;
        }

        float sRGB(float linear) {
            if (linear > 1.0) {
                return 1.0;
            } else if (linear < 0.0) {
                return 0.0;
            } else if (linear < 0.0031308) {
                return 12.92 * linear;
            } else {
                return 1.055 * pow(linear, 0.41666) - 0.055;
            }
        }

        vec3 applyTonemap(vec3 col) {
            switch (tonemap) {
                case SRGB:        return vec3(sRGB(col.r), sRGB(col.g), sRGB(col.b));
                case GAMMA:       return pow(col, vec3(1.0 / 2.2));
                // Here grayscale is compressed such that a value of 1/1000th becomes 0 and 1000 becomes 1.
                // Due to the usage of the logarithm, varying the exposure simply has an additive effect on
                // the result of the following expression, and therefore the high range of values (a factor
                // of ~1,000,000 is preserved).
                case FALSE_COLOR: return falseColor(log2(average(col)) / 20.0 + 0.5);
                case POS_NEG:     return vec3(-average(min(col, vec3(0.0))) / 0.5, average(max(col, vec3(0.0))) / 0.5, 0.0);
            }
            return vec3(0.0);
        }

        vec3 applyMetric(vec3 col, vec3 reference) {
            switch (metric) {
                case ERROR:                   return col;
                case ABSOLUTE_ERROR:          return abs(col);
                case SQUARED_ERROR:           return col * col;
                case RELATIVE_ABSOLUTE_ERROR: return abs(col) / (reference + vec3(0.01));
                case RELATIVE_SQUARED_ERROR:  return col * col / (reference * reference + vec3(0.0001));
            }
            return vec3(0.0);
        }

        void main() {
            vec4 image = vec4(
                texture(imageRed, imageUv).x,
                texture(imageGreen, imageUv).x,
                texture(imageBlue, imageUv).x,
                texture(imageAlpha, imageUv).x
            );

            if (!hasReference) {
                color = vec4(applyTonemap(applyExposureAndOffset(image.rgb)), image.a);
                return;
            }

            vec4 reference = vec4(
                texture(referenceRed, referenceUv).x,
                texture(referenceGreen, referenceUv).x,
                texture(referenceBlue, referenceUv).x,
                texture(referenceAlpha, referenceUv).x
            );

            vec3 difference = image.rgb - reference.rgb;
            float alpha = (image.a + reference.a) * 0.5;
            color = vec4(applyTonemap(applyExposureAndOffset(applyMetric(difference, reference.rgb))), alpha);
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

UberShader::~UberShader() {
    mShader.free();
}

void UberShader::draw(
    std::array<const GlTexture*, 4> texturesImage,
    const Eigen::Matrix3f& transformImage,
    std::array<const GlTexture*, 4> texturesReference,
    const Eigen::Matrix3f& transformReference,
    float exposure,
    float offset,
    ETonemap tonemap,
    EMetric metric
) {
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, texturesImage[i]->id());
    }

    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + 4 + i);
        glBindTexture(GL_TEXTURE_2D, texturesReference[i]->id());
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mShader.bind();
    mShader.setUniform("imageRed",   0);
    mShader.setUniform("imageGreen", 1);
    mShader.setUniform("imageBlue",  2);
    mShader.setUniform("imageAlpha", 3);
    mShader.setUniform("imageTransform", transformImage);

    mShader.setUniform("referenceRed",   4);
    mShader.setUniform("referenceGreen", 5);
    mShader.setUniform("referenceBlue",  6);
    mShader.setUniform("referenceAlpha", 7);
    mShader.setUniform("referenceTransform", transformReference);

    mShader.setUniform("hasReference", true);

    mShader.setUniform("exposure", exposure);
    mShader.setUniform("offset", offset);
    mShader.setUniform("tonemap", static_cast<int>(tonemap));
    mShader.setUniform("metric", static_cast<int>(metric));

    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

void UberShader::draw(
    std::array<const GlTexture*, 4> texturesImage,
    const Eigen::Matrix3f& transformImage,
    float exposure,
    float offset,
    ETonemap tonemap
) {
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, texturesImage[i]->id());
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mShader.bind();
    mShader.setUniform("imageRed", 0);
    mShader.setUniform("imageGreen", 1);
    mShader.setUniform("imageBlue", 2);
    mShader.setUniform("imageAlpha", 3);
    mShader.setUniform("imageTransform", transformImage);

    mShader.setUniform("hasReference", false);

    mShader.setUniform("exposure", exposure);
    mShader.setUniform("offset", offset);
    mShader.setUniform("tonemap", static_cast<int>(tonemap));

    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

TEV_NAMESPACE_END
