// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/FalseColor.h>
#include <tev/UberShader.h>

#include <Eigen/Dense>

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

UberShader::UberShader(nanogui::RenderPass* renderPass)
: mColorMap{GL_CLAMP_TO_EDGE, GL_LINEAR, false}
{
    mShader = new nanogui::Shader{
        renderPass,
        "ubershader",

        // Vertex shader
        R"(#version 330

        uniform vec2 pixelSize;
        uniform vec2 checkerSize;

        uniform mat3 imageTransform;
        uniform mat3 referenceTransform;

        in vec2 position;

        out vec2 checkerUv;
        out vec2 imageUv;
        out vec2 referenceUv;

        void main() {
            checkerUv = position / (pixelSize * checkerSize);
            imageUv = (imageTransform * vec3(position, 1.0)).xy;
            referenceUv = (referenceTransform * vec3(position, 1.0)).xy;

            gl_Position = vec4(position, 1.0, 1.0);
        })",

        // Fragment shader
        R"(#version 330

        #define SRGB        0
        #define GAMMA       1
        #define FALSE_COLOR 2
        #define POS_NEG     3

        #define ERROR                   0
        #define ABSOLUTE_ERROR          1
        #define SQUARED_ERROR           2
        #define RELATIVE_ABSOLUTE_ERROR 3
        #define RELATIVE_SQUARED_ERROR  4

        uniform sampler2D image;
        uniform bool hasImage;

        uniform sampler2D reference;
        uniform bool hasReference;

        uniform sampler2D colormap;

        uniform float exposure;
        uniform float offset;
        uniform float gamma;
        uniform int tonemap;
        uniform int metric;

        uniform vec4 bgColor;

        in vec2 checkerUv;
        in vec2 imageUv;
        in vec2 referenceUv;

        out vec4 color;

        float average(vec3 col) {
            return (col.r + col.g + col.b) / 3.0;
        }

        vec3 applyExposureAndOffset(vec3 col) {
            return pow(2.0, exposure) * col + offset;
        }

        vec3 applyInverseExposureAndOffset(vec3 col) {
            return pow(2.0, -exposure) * (col - offset);
        }

        vec3 falseColor(float v) {
            v = clamp(v, 0.0, 1.0);
            return texture(colormap, vec2(v, 0.5)).rgb;
        }

        float linear(float sRGB) {
            if (sRGB > 1.0) {
                return 1.0;
            } else if (sRGB < 0.0) {
                return 0.0;
            } else if (sRGB <= 0.04045) {
                return sRGB / 12.92;
            } else {
                return pow((sRGB + 0.055) / 1.055, 2.4);
            }
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

        vec3 applyTonemap(vec3 col, vec4 background) {
            switch (tonemap) {
                case SRGB:
                    col = col +
                        (vec3(linear(background.r), linear(background.g), linear(background.b)) - offset) * background.a;
                    return vec3(sRGB(col.r), sRGB(col.g), sRGB(col.b));
                case GAMMA:
                    col = col + (pow(background.rgb, vec3(gamma)) - offset) * background.a;
                    return pow(col, vec3(1.0 / gamma));
                // Here grayscale is compressed such that the darkest color is is 1/1024th as bright as the brightest color.
                case FALSE_COLOR:
                    return falseColor(log2(average(col)+0.03125) / 10.0 + 0.5) + (background.rgb - falseColor(0.0)) * background.a;
                case POS_NEG:
                    return vec3(-average(min(col, vec3(0.0))) * 2.0, average(max(col, vec3(0.0))) * 2.0, 0.0) + background.rgb * background.a;
            }
            return vec3(0.0);
        }

        vec3 applyMetric(vec3 col, vec3 reference) {
            switch (metric) {
                case ERROR:                   return col;
                case ABSOLUTE_ERROR:          return abs(col);
                case SQUARED_ERROR:           return col * col;
                case RELATIVE_ABSOLUTE_ERROR: return abs(col) / (reference + vec3(0.01));
                case RELATIVE_SQUARED_ERROR:  return col * col / (reference * reference + vec3(0.01));
            }
            return vec3(0.0);
        }

        vec4 sample(sampler2D sampler, vec2 uv) {
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                return vec4(0.0);
            }
            return texture(sampler, uv);
        }

        void main() {
            vec3 darkGray = vec3(0.5, 0.5, 0.5);
            vec3 lightGray = vec3(0.55, 0.55, 0.55);

            vec3 checker = mod(int(floor(checkerUv.x) + floor(checkerUv.y)), 2) == 0 ? darkGray : lightGray;
            checker = bgColor.rgb * bgColor.a + checker * (1.0 - bgColor.a);
            if (!hasImage) {
                color = vec4(checker, 1.0);
                return;
            }

            vec4 imageVal = sample(image, imageUv);
            if (!hasReference) {
                color = vec4(
                    applyTonemap(applyExposureAndOffset(imageVal.rgb), vec4(checker, 1.0 - imageVal.a)),
                    1.0
                );
                return;
            }

            vec4 referenceVal = sample(reference, referenceUv);

            vec3 difference = imageVal.rgb - referenceVal.rgb;
            float alpha = (imageVal.a + referenceVal.a) * 0.5;
            color = vec4(
                applyTonemap(applyExposureAndOffset(applyMetric(difference, referenceVal.rgb)), vec4(checker, 1.0 - alpha)),
                1.0
            );
        })"
    };

    // 2 Triangles
    uint32_t indices[3*2] = {
        0, 1, 2,
        2, 3, 0
    };
    float positions[3*4] = {
        -1, -1, 0,
        1, -1, 0,
        1, 1, 0,
        -1, 1, 0,
    };

    mShader->set_buffer("indices", nanogui::VariableType::UInt32, {3*2}, indices);
    mShader->set_buffer("position", nanogui::VariableType::Float32, {4, 3}, positions);

    const auto& fcd = colormap::turbo();
    mColorMap.setData(fcd, Vector2i{(int)fcd.size() / 4, 1}, 4);
}

UberShader::~UberShader() { }

void UberShader::draw(const Vector2f& pixelSize, const Vector2f& checkerSize) {
    bindCheckerboardData(pixelSize, checkerSize);
    mShader->set_uniform("hasImage", false);
    mShader->set_uniform("hasReference", false);

    mShader->begin();
    mShader->draw_array(nanogui::Shader::PrimitiveType::Triangle, 0, 6, true);
    mShader->end();
}

void UberShader::draw(
    const Vector2f& pixelSize,
    const Vector2f& checkerSize,
    GlTexture* textureImage,
    const Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap
) {
    bindCheckerboardData(pixelSize, checkerSize);
    bindImageData(textureImage, transformImage, exposure, offset, gamma, tonemap);
    mShader->set_uniform("hasImage", true);
    mShader->set_uniform("hasReference", false);

    mShader->begin();
    mShader->draw_array(nanogui::Shader::PrimitiveType::Triangle, 0, 6, true);
    mShader->end();
}

void UberShader::draw(
    const Vector2f& pixelSize,
    const Vector2f& checkerSize,
    GlTexture* textureImage,
    const Matrix3f& transformImage,
    GlTexture* textureReference,
    const Matrix3f& transformReference,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap,
    EMetric metric
) {
    bindCheckerboardData(pixelSize, checkerSize);
    bindImageData(textureImage, transformImage, exposure, offset, gamma, tonemap);
    bindReferenceData(textureReference, transformReference, metric);
    mShader->set_uniform("hasImage", true);
    mShader->set_uniform("hasReference", true);

    mShader->begin();
    mShader->draw_array(nanogui::Shader::PrimitiveType::Triangle, 0, 6, true);
    mShader->end();
}

void UberShader::bindCheckerboardData(const Vector2f& pixelSize, const Vector2f& checkerSize) {
    mShader->set_uniform("pixelSize", pixelSize);
    mShader->set_uniform("checkerSize", checkerSize);
    mShader->set_uniform("bgColor", mBackgroundColor);
}

void UberShader::bindImageData(
    GlTexture* textureImage,
    const Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap
) {
    glActiveTexture(GL_TEXTURE0);
    textureImage->bind();

    mShader->set_uniform("image", 0);
    mShader->set_uniform("imageTransform", transformImage);

    mShader->set_uniform("exposure", exposure);
    mShader->set_uniform("offset", offset);
    mShader->set_uniform("gamma", gamma);
    mShader->set_uniform("tonemap", static_cast<int>(tonemap));

    glActiveTexture(GL_TEXTURE2);
    mColorMap.bind();
    mShader->set_uniform("colormap", 2);
}

void UberShader::bindReferenceData(
    GlTexture* textureReference,
    const Matrix3f& transformReference,
    EMetric metric
) {
    glActiveTexture(GL_TEXTURE1);
    textureReference->bind();

    mShader->set_uniform("reference", 1);
    mShader->set_uniform("referenceTransform", transformReference);

    mShader->set_uniform("metric", static_cast<int>(metric));
}

TEV_NAMESPACE_END
