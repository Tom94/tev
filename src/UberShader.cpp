// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/FalseColor.h>
#include <tev/UberShader.h>

#include <Eigen/Dense>

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

UberShader::UberShader(nanogui::RenderPass* renderPass) {
    try {
        mShader = new nanogui::Shader{
            renderPass,
            "ubershader",

#if defined(NANOGUI_USE_OPENGL)
            // Vertex shader
            R"(#version 330

            uniform vec2 pixelSize;
            uniform vec2 checkerSize;

            uniform vec2 imageScale;
            uniform vec2 imageOffset;

            uniform vec2 referenceScale;
            uniform vec2 referenceOffset;

            in vec2 position;

            out vec2 checkerUv;
            out vec2 imageUv;
            out vec2 referenceUv;

            void main() {
                checkerUv = position / (pixelSize * checkerSize);
                imageUv = position * imageScale + imageOffset;
                referenceUv = position * referenceScale + referenceOffset;

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
            uniform bool clipToLdr;
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

            vec3 falseColor(float v) {
                v = clamp(v, 0.0, 1.0);
                return texture(colormap, vec2(v, 0.5)).rgb;
            }

            float linear(float sRGB) {
                float outSign = sign(sRGB);
                sRGB = abs(sRGB);

                if (sRGB <= 0.04045) {
                    return outSign * sRGB / 12.92;
                } else {
                    return outSign * pow((sRGB + 0.055) / 1.055, 2.4);
                }
            }

            float sRGB(float linear) {
                float outSign = sign(linear);
                linear = abs(linear);

                if (linear < 0.0031308f) {
                    return outSign * 12.92 * linear;
                } else {
                    return outSign * 1.055 * pow(linear, 0.41666) - 0.055;
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
                        return sign(col) * pow(abs(col), vec3(1.0 / gamma));
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
                vec4 color = texture(sampler, uv);
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                    color = vec4(0.0);
                }
                return color;
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

                    if (clipToLdr) {
                        color.rgb = clamp(color.rgb, 0.0, 1.0);
                    }

                    return;
                }

                vec4 referenceVal = sample(reference, referenceUv);

                vec3 difference = imageVal.rgb - referenceVal.rgb;
                float alpha = (imageVal.a + referenceVal.a) * 0.5;
                color = vec4(
                    applyTonemap(applyExposureAndOffset(applyMetric(difference, referenceVal.rgb)), vec4(checker, 1.0 - alpha)),
                    1.0
                );

                if (clipToLdr) {
                    color.rgb = clamp(color.rgb, 0.0, 1.0);
                }
            })"
#elif defined(NANOGUI_USE_GLES)
            "", "" // TODO: write
#elif defined(NANOGUI_USE_METAL)
            // Vertex shader
            R"(using namespace metal;

            struct VertexOut {
                float4 position [[position]];
                float2 checkerUv;
                float2 imageUv;
                float2 referenceUv;
            };

            vertex VertexOut vertex_main(
                const device packed_float2* position,
                const constant float2& pixelSize,
                const constant float2& checkerSize,
                const constant float2& imageScale,
                const constant float2& imageOffset,
                const constant float2& referenceScale,
                const constant float2& referenceOffset,
                uint id [[vertex_id]]
            ) {
                VertexOut vert;
                vert.position = float4(position[id], 1.0f, 1.0f);
                vert.checkerUv = position[id] / (pixelSize * checkerSize);

                vert.imageUv = position[id] * imageScale + imageOffset;
                vert.referenceUv = position[id] * referenceScale + referenceOffset;
                return vert;
            })",

            // Fragment shader
            R"(using namespace metal;

            #define SRGB        0
            #define GAMMA       1
            #define FALSE_COLOR 2
            #define POS_NEG     3

            #define ERROR                   0
            #define ABSOLUTE_ERROR          1
            #define SQUARED_ERROR           2
            #define RELATIVE_ABSOLUTE_ERROR 3
            #define RELATIVE_SQUARED_ERROR  4

            float average(float3 col) {
                return (col.r + col.g + col.b) / 3.0f;
            }

            float3 applyExposureAndOffset(float3 col, float exposure, float offset) {
                return pow(2.0f, exposure) * col + offset;
            }

            float3 falseColor(float v, texture2d<float, access::sample> colormap, sampler colormapSampler) {
                v = clamp(v, 0.0f, 1.0f);
                return colormap.sample(colormapSampler, float2(v, 0.5f)).rgb;
            }

            float linear(float sRGB) {
                float outSign = sign(sRGB);
                sRGB = abs(sRGB);

                if (sRGB <= 0.04045f) {
                    return outSign * sRGB / 12.92f;
                } else {
                    return outSign * pow((sRGB + 0.055f) / 1.055f, 2.4f);
                }
            }

            float sRGB(float linear) {
                float outSign = sign(linear);
                linear = abs(linear);

                if (linear < 0.0031308f) {
                    return outSign * 12.92f * linear;
                } else {
                    return outSign * 1.055f * pow(linear, 0.41666f) - 0.055f;
                }
            }

            float3 applyTonemap(float3 col, float4 background, int tonemap, float offset, float gamma, texture2d<float, access::sample> colormap, sampler colormapSampler) {
                switch (tonemap) {
                    case SRGB:
                        col = col +
                            (float3(linear(background.r), linear(background.g), linear(background.b)) - offset) * background.a;
                        return float3(sRGB(col.r), sRGB(col.g), sRGB(col.b));
                    case GAMMA:
                        col = col + (pow(background.rgb, float3(gamma)) - offset) * background.a;
                        return sign(col) * pow(abs(col), float3(1.0 / gamma));
                    // Here grayscale is compressed such that the darkest color is is 1/1024th as bright as the brightest color.
                    case FALSE_COLOR:
                        return falseColor(log2(average(col)+0.03125f) / 10.0f + 0.5f, colormap, colormapSampler) + (background.rgb - falseColor(0.0f, colormap, colormapSampler)) * background.a;
                    case POS_NEG:
                        return float3(-average(min(col, float3(0.0f))) * 2.0f, average(max(col, float3(0.0f))) * 2.0f, 0.0f) + background.rgb * background.a;
                }
                return float3(0.0f);
            }

            float3 applyMetric(float3 col, float3 reference, int metric) {
                switch (metric) {
                    case ERROR:                   return col;
                    case ABSOLUTE_ERROR:          return abs(col);
                    case SQUARED_ERROR:           return col * col;
                    case RELATIVE_ABSOLUTE_ERROR: return abs(col) / (reference + float3(0.01f));
                    case RELATIVE_SQUARED_ERROR:  return col * col / (reference * reference + float3(0.01f));
                }
                return float3(0.0f);
            }

            float4 sample(texture2d<float, access::sample> texture, sampler textureSampler, float2 uv) {
                float4 color = texture.sample(textureSampler, uv);
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
                    return float4(0.0f);
                }
                return color;
            }

            struct VertexOut {
                float4 position [[position]];
                float2 checkerUv;
                float2 imageUv;
                float2 referenceUv;
            };

            fragment float4 fragment_main(
                VertexOut vert [[stage_in]],
                texture2d<float, access::sample> image,
                sampler image_sampler,
                texture2d<float, access::sample> reference,
                sampler reference_sampler,
                texture2d<float, access::sample> colormap,
                sampler colormap_sampler,
                const constant bool& hasImage,
                const constant bool& hasReference,
                const constant float& exposure,
                const constant float& offset,
                const constant float& gamma,
                const constant bool& clipToLdr,
                const constant int& tonemap,
                const constant int& metric,
                const constant float4& bgColor
            ) {
                float3 darkGray = float3(0.5f, 0.5f, 0.5f);
                float3 lightGray = float3(0.55f, 0.55f, 0.55f);

                float3 checker = int(floor(vert.checkerUv.x) + floor(vert.checkerUv.y)) % 2 == 0 ? darkGray : lightGray;
                checker = bgColor.rgb * bgColor.a + checker * (1.0f - bgColor.a);
                if (!hasImage) {
                    return float4(checker, 1.0f);
                }

                float4 imageVal = sample(image, image_sampler, vert.imageUv);
                if (!hasReference) {
                    float4 color = float4(
                        applyTonemap(
                            applyExposureAndOffset(imageVal.rgb, exposure, offset),
                            float4(checker, 1.0f - imageVal.a),
                            tonemap,
                            offset,
                            gamma,
                            colormap,
                            colormap_sampler
                        ),
                        1.0f
                    );
                    if (clipToLdr) {
                        color.rgb = clamp(color.rgb, 0.0f, 1.0f);
                    }
                    return color;
                }

                float4 referenceVal = sample(reference, reference_sampler, vert.referenceUv);

                float3 difference = imageVal.rgb - referenceVal.rgb;
                float alpha = (imageVal.a + referenceVal.a) * 0.5f;
                float4 color = float4(
                    applyTonemap(
                        applyExposureAndOffset(applyMetric(difference, referenceVal.rgb, metric), exposure, offset),
                        float4(checker, 1.0f - alpha),
                        tonemap,
                        offset,
                        gamma,
                        colormap,
                        colormap_sampler
                    ),
                    1.0f
                );
                if (clipToLdr) {
                    color.rgb = clamp(color.rgb, 0.0f, 1.0f);
                }
                return color;
            })"
#endif
        };
    } catch (const runtime_error& e) {
        tlog::error() << tfm::format("Unable to compile shader: %s", e.what());
    }

    // 2 Triangles
    uint32_t indices[3*2] = {
        0, 1, 2,
        2, 3, 0,
    };
    float positions[2*4] = {
        -1.f, -1.f,
        1.f, -1.f,
        1.f, 1.f,
        -1.f, 1.f,
    };

    mShader->set_buffer("indices", nanogui::VariableType::UInt32, {3*2}, indices);
    mShader->set_buffer("position", nanogui::VariableType::Float32, {4, 2}, positions);

    const auto& fcd = colormap::turbo();

    mColorMap = new nanogui::Texture{
        nanogui::Texture::PixelFormat::RGBA,
        nanogui::Texture::ComponentFormat::Float32,
        nanogui::Vector2i{(int)fcd.size() / 4, 1}
    };
    mColorMap->upload((uint8_t*)fcd.data());
}

UberShader::~UberShader() { }

void UberShader::draw(const Vector2f& pixelSize, const Vector2f& checkerSize) {
    draw(
        pixelSize, checkerSize,
        nullptr, nanogui::Matrix3f{0.0f},
        0.0f, 0.0f, 0.0f, false,
        ETonemap::SRGB
    );
}

void UberShader::draw(
    const Vector2f& pixelSize,
    const Vector2f& checkerSize,
    nanogui::Texture* textureImage,
    const nanogui::Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    bool clipToLdr,
    ETonemap tonemap
) {
    draw(
        pixelSize, checkerSize,
        textureImage, transformImage,
        nullptr, nanogui::Matrix3f{0.0f},
        exposure, offset, gamma, clipToLdr,
        tonemap, EMetric::Error
    );
}

void UberShader::draw(
    const Vector2f& pixelSize,
    const Vector2f& checkerSize,
    nanogui::Texture* textureImage,
    const nanogui::Matrix3f& transformImage,
    nanogui::Texture* textureReference,
    const nanogui::Matrix3f& transformReference,
    float exposure,
    float offset,
    float gamma,
    bool clipToLdr,
    ETonemap tonemap,
    EMetric metric
) {
    bool hasImage = textureImage;
    if (!hasImage) {
        // Just to have _some_ valid texture to bind. Will be ignored.
        textureImage = mColorMap.get();
    }

    bool hasReference = textureReference;
    if (!hasReference) {
        // Just to have _some_ valid texture to bind. Will be ignored.
        textureReference = textureImage;
    }

    bindCheckerboardData(pixelSize, checkerSize);
    bindImageData(textureImage, transformImage, exposure, offset, gamma, tonemap);
    bindReferenceData(textureReference, transformReference, metric);
    mShader->set_uniform("hasImage", hasImage);
    mShader->set_uniform("hasReference", hasReference);
    mShader->set_uniform("clipToLdr", clipToLdr);

    mShader->begin();
    mShader->draw_array(nanogui::Shader::PrimitiveType::Triangle, 0, 6, true);
    mShader->end();
}

void UberShader::bindCheckerboardData(const Vector2f& pixelSize, const Vector2f& checkerSize) {
    mShader->set_uniform("pixelSize", nanogui::Vector2f{pixelSize.x(), pixelSize.y()});
    mShader->set_uniform("checkerSize", nanogui::Vector2f{checkerSize.x(), checkerSize.y()});
    mShader->set_uniform("bgColor", mBackgroundColor);
}

void UberShader::bindImageData(
    nanogui::Texture* textureImage,
    const nanogui::Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap
) {
    mShader->set_texture("image", textureImage);
    mShader->set_uniform("imageScale", nanogui::Vector2f{transformImage.m[0][0], transformImage.m[1][1]});
    mShader->set_uniform("imageOffset", nanogui::Vector2f{transformImage.m[2][0], transformImage.m[2][1]});

    mShader->set_uniform("exposure", exposure);
    mShader->set_uniform("offset", offset);
    mShader->set_uniform("gamma", gamma);
    mShader->set_uniform("tonemap", static_cast<int>(tonemap));

    mShader->set_texture("colormap", mColorMap.get());
}

void UberShader::bindReferenceData(
    nanogui::Texture* textureReference,
    const nanogui::Matrix3f& transformReference,
    EMetric metric
) {
    mShader->set_texture("reference", textureReference);
    mShader->set_uniform("referenceScale", nanogui::Vector2f{transformReference.m[0][0], transformReference.m[1][1]});
    mShader->set_uniform("referenceOffset", nanogui::Vector2f{transformReference.m[2][0], transformReference.m[2][1]});

    mShader->set_uniform("metric", static_cast<int>(metric));
}

TEV_NAMESPACE_END
