/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/FalseColor.h>
#include <tev/Image.h>
#include <tev/UberShader.h>

using namespace nanogui;
using namespace std;

namespace tev {

static constexpr size_t DITHER_MATRIX_SIZE = 8;
using DitherMatrix = array<float, DITHER_MATRIX_SIZE * DITHER_MATRIX_SIZE>;

static DitherMatrix ditherMatrix(float scale) {
    // 8x8 Bayer dithering matrix scaled to [-0.5f, 0.5f] / 255
    DitherMatrix mat = {
        {0, 32, 8,  40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
         3, 35, 11, 43, 1, 33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21}
    };

    for (size_t i = 0; i < DITHER_MATRIX_SIZE * DITHER_MATRIX_SIZE; ++i) {
        mat[i] = (mat[i] / DITHER_MATRIX_SIZE / DITHER_MATRIX_SIZE - 0.5f) * scale;
    }

    return mat;
}

enum class EShaderChannelConfig : int { R = 0, RG, RGB, RA, RGA, RGBA };

UberShader::UberShader(RenderPass* renderPass, float ditherScale) {
    try {
#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
#    if defined(NANOGUI_USE_OPENGL)
        const string preamble = R"(#version 110)";
#    elif defined(NANOGUI_USE_GLES)
        const string preamble =
            R"(#version 100
        precision highp float;)";
#    endif
        const auto vertexShader = preamble +
            R"glsl(
            uniform vec2 pixelSize;
            uniform vec2 checkerSize;
            uniform float ditherSize;

            uniform vec2 imageScale;
            uniform vec2 imageOffset;

            uniform vec2 referenceScale;
            uniform vec2 referenceOffset;

            attribute vec2 position;

            varying vec2 checkerUv;
            varying vec2 ditherUv;

            varying vec2 imageUv;
            varying vec2 referenceUv;

            void main() {
                // The offset of 0.25 is necessary to avoid sampling exact pixel borders. The offset is 0.25 rather than 0.5, because the
                // domain of the screen shader is [-1, 1] rather than [0, 1].
                checkerUv = (position / pixelSize + 0.25) / checkerSize;
                ditherUv = (position / pixelSize + 0.25) / ditherSize;

                imageUv = position * imageScale + imageOffset;
                referenceUv = position * referenceScale + referenceOffset;

                gl_Position = vec4(position, 1.0, 1.0);
            })glsl";

        const auto fragmentShader = preamble +
            R"glsl(
            #define GAMMA       0
            #define FALSE_COLOR 1
            #define POS_NEG     2

            #define ERROR                   0
            #define ABSOLUTE_ERROR          1
            #define SQUARED_ERROR           2
            #define RELATIVE_ABSOLUTE_ERROR 3
            #define RELATIVE_SQUARED_ERROR  4

            #define CHANNEL_CONFIG_R    0
            #define CHANNEL_CONFIG_RG   1
            #define CHANNEL_CONFIG_RGB  2
            #define CHANNEL_CONFIG_RA   3
            #define CHANNEL_CONFIG_RGA  4
            #define CHANNEL_CONFIG_RGBA 5

            uniform sampler2D image;
            uniform bool hasImage;

            uniform sampler2D reference;
            uniform bool hasReference;

            uniform int channelConfig;

            uniform sampler2D colormap;
            uniform sampler2D ditherMatrix;

            uniform float exposure;
            uniform float offset;
            uniform float gamma;
            uniform float colorMultiplier;
            uniform float brightnessLimit;
            uniform float rolloffStops;
            uniform bool clipToLdr;
            uniform int tonemap;
            uniform int metric;
            uniform vec4 channelMask;

            uniform vec2 cropMin;
            uniform vec2 cropMax;

            uniform float splitscreenEnabled;
            uniform float splitscreenSliderT;

            uniform vec4 bgColor;

            varying vec2 checkerUv;
            varying vec2 ditherUv;

            varying vec2 imageUv;
            varying vec2 referenceUv;

            float average(vec3 col) {
                return (col.r + col.g + col.b) / 3.0;
            }

            vec3 applyExposureAndOffset(vec3 col) {
                return pow(2.0, exposure) * col + offset;
            }

            vec3 falseColor(float v) {
                v = clamp(v, 0.0, 1.0);
                return texture2D(colormap, vec2(v, 0.5)).rgb;
            }

            vec3 mixb(vec3 a, vec3 b, bvec3 mask) {
                return mix(a, b, vec3(mask));
            }

            vec3 applyGamma(vec3 col, float gamma) {
                return sign(col) * pow(abs(col), vec3(gamma));
            }

            vec3 applyTonemap(vec3 col, vec4 background) {
                if (tonemap == GAMMA) {
                    col = col + (pow(background.rgb, vec3(gamma)) - offset) * background.a;
                    return applyGamma(col, 1.0 / gamma);
                } else if (tonemap == FALSE_COLOR) {
                    return falseColor(log2(average(col)+0.03125) / 10.0 + 0.5) + (background.rgb - falseColor(0.0)) * background.a;
                } else if (tonemap == POS_NEG) {
                    return vec3(-average(min(col, vec3(0.0))) * 2.0, average(max(col, vec3(0.0))) * 2.0, 0.0) + background.rgb * background.a;
                }

                return vec3(0.0);
            }

            vec3 applyMetric(vec3 col, vec3 reference) {
                if (metric == ERROR) {
                    return col;
                } else if (metric == ABSOLUTE_ERROR) {
                    return abs(col);
                } else if (metric == SQUARED_ERROR) {
                    return col * col;
                } else if (metric == RELATIVE_ABSOLUTE_ERROR) {
                    return abs(col) / (reference + vec3(0.01));
                } else if (metric == RELATIVE_SQUARED_ERROR) {
                    return col * col / (reference * reference + vec3(0.01));
                }

                return vec3(0.0);
            }

            vec4 applyMask(vec4 color, vec4 mask) {
                return color * mask + vec4(0.0, 0.0, 0.0, 1.0) * (vec4(1.0) - mask);
            }

            vec4 sample(sampler2D sampler, vec4 mask, vec2 uv) {
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                    return vec4(0.0);
                }

                vec4 color = texture2D(sampler, uv);

                // Duplicate first channel in monochromatic images and move alpha to end if not already there.
                if (channelConfig == CHANNEL_CONFIG_R) {
                    color = vec4(color.r, color.r, color.r, 1.0);
                } else if (channelConfig == CHANNEL_CONFIG_RA) {
                    color = vec4(color.r, color.r, color.r, color.g);
                } else if (channelConfig == CHANNEL_CONFIG_RGA) {
                    color = vec4(color.r, color.g, 0.0, color.b);
                }

                return applyMask(color, mask);
            }

            vec4 dither(vec4 color) {
                color.rgb += texture2D(ditherMatrix, fract(ditherUv)).r;
                return color;
            }

            vec3 limitBrightness(vec3 x, float logHi, float shoulderStops) {
                float shoulderStart = logHi - shoulderStops;
                vec3 logx = log2(max(x, vec3(1e-6)));
                vec3 t = (logx - shoulderStart) / shoulderStops;
                vec3 s = exp(-t);
                vec3 compressed = exp2(logHi - shoulderStops * s);
                return mixb(compressed, x, lessThanEqual(logx, vec3(shoulderStart)));
            }

            vec3 smoothClamp(vec3 color) {
                color = applyGamma(limitBrightness(applyGamma(color, 2.2), brightnessLimit, rolloffStops), 1.0 / 2.2);

                float minVal = clipToLdr ? 0.0 : -64.0;
                float maxVal = clipToLdr ? 1.0 : 64.0;
                return clamp(color, vec3(minVal), vec3(maxVal));
            }

            vec4 computeColor() {
                vec3 darkGray = vec3(0.5, 0.5, 0.5);
                vec3 lightGray = vec3(0.55, 0.55, 0.55);
                vec3 checker = abs(mod(floor(checkerUv.x) + floor(checkerUv.y), 2.0)) < 0.5 ? darkGray : lightGray;

                float cropAlpha = imageUv.x < cropMin.x || imageUv.x > cropMax.x || imageUv.y < cropMin.y || imageUv.y > cropMax.y ? 0.3 : 1.0;

                vec4 val = vec4(0.0);
                if (hasImage) {
                    val = sample(image, channelMask, imageUv);
                    val.a *= cropAlpha;
                }

                if (hasReference) {
                    vec4 referenceVal = sample(reference, channelMask, referenceUv);
                    referenceVal.a *= cropAlpha;

                    vec3 difference = val.rgb - referenceVal.rgb;
                    vec4 errorMetric = vec4(applyMetric(difference, referenceVal.rgb), (val.a + referenceVal.a) * 0.5);
                    vec4 splitscreen = mix(val, referenceVal, vec4(step(splitscreenSliderT, imageUv.x)));
                    val = mix(errorMetric, splitscreen, splitscreenEnabled);
                }

                val += bgColor * (1.0 - val.a);
                vec4 result = vec4(
                    applyTonemap(colorMultiplier * applyExposureAndOffset(val.rgb), vec4(checker, 1.0 - val.a)),
                    1.0
                );

                result.rgb = smoothClamp(result.rgb);
                return result;
            }

            void main() {
                vec4 color = computeColor();
                gl_FragColor = dither(color);
            })glsl";
#elif defined(NANOGUI_USE_METAL)
        auto vertexShader =
            R"(using namespace metal;

            struct VertexOut {
                float4 position [[position]];
                float2 checkerUv;
                float2 ditherUv;
                float2 imageUv;
                float2 referenceUv;
            };

            vertex VertexOut vertex_main(
                const device packed_float2* position,
                const constant float2& pixelSize,
                const constant float2& checkerSize,
                const constant float& ditherSize,
                const constant float2& imageScale,
                const constant float2& imageOffset,
                const constant float2& referenceScale,
                const constant float2& referenceOffset,
                uint id [[vertex_id]]
            ) {
                VertexOut vert;
                vert.position = float4(position[id], 1.0f, 1.0f);

                // The offset of 0.25 is necessary to avoid sampling exact pixel borders. The offset is 0.25 rather than 0.5, because the
                // domain of the screen shader is [-1, 1] rather than [0, 1].
                vert.checkerUv = (position[id] / pixelSize + 0.25) / checkerSize;
                vert.ditherUv = (position[id] / pixelSize + 0.25) / ditherSize;

                vert.imageUv = position[id] * imageScale + imageOffset;
                vert.referenceUv = position[id] * referenceScale + referenceOffset;
                return vert;
            })";

        auto fragmentShader =
            R"(using namespace metal;

            #define GAMMA       0
            #define FALSE_COLOR 1
            #define POS_NEG     2

            #define ERROR                   0
            #define ABSOLUTE_ERROR          1
            #define SQUARED_ERROR           2
            #define RELATIVE_ABSOLUTE_ERROR 3
            #define RELATIVE_SQUARED_ERROR  4

            #define CHANNEL_CONFIG_R    0
            #define CHANNEL_CONFIG_RG   1
            #define CHANNEL_CONFIG_RGB  2
            #define CHANNEL_CONFIG_RA   3
            #define CHANNEL_CONFIG_RGA  4
            #define CHANNEL_CONFIG_RGBA 5

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

            float3 applyGamma(float3 col, float gamma) {
                return sign(col) * pow(abs(col), float3(gamma));
            }

            float3 applyTonemap(float3 col, float4 background, int tonemap, float offset, float gamma, texture2d<float, access::sample> colormap, sampler colormapSampler) {
                switch (tonemap) {
                    case GAMMA:
                        col = col + (pow(background.rgb, float3(gamma)) - offset) * background.a;
                        return applyGamma(col, 1.0f / gamma);
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

            float4 applyMask(float4 color, float4 mask) {
                return color * mask + float4(0.0f, 0.0f, 0.0f, 1.0f) * (float4(1.0f) - mask);
            }

            float4 sample(texture2d<float, access::sample> texture, sampler textureSampler, float4 mask, float2 uv, int channelConfig) {
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
                    return float4(0.0f);
                }

                float4 color = texture.sample(textureSampler, uv);

                // Duplicate first channel in monochromatic images and move alpha to end if not already there.
                if (channelConfig == CHANNEL_CONFIG_R) {
                    color = float4(color.r, color.r, color.r, 1.0f);
                } else if (channelConfig == CHANNEL_CONFIG_RA) {
                    color = float4(color.r, color.r, color.r, color.g);
                } else if (channelConfig == CHANNEL_CONFIG_RGA) {
                    color = float4(color.r, color.g, 0.0f, color.b);
                }

                return applyMask(color, mask);
            }

            struct VertexOut {
                float4 position [[position]];
                float2 checkerUv;
                float2 ditherUv;
                float2 imageUv;
                float2 referenceUv;
            };

            float4 dither(float4 color, texture2d<float, access::sample> ditherMatrix, sampler ditherMatrixSampler, float2 ditherUv) {
                color.rgb += ditherMatrix.sample(ditherMatrixSampler, fract(ditherUv)).r;
                return color;
            }

            float3 limitBrightness(float3 x, float logHi, float shoulderStops) {
                const float shoulderStart = logHi - shoulderStops;
                const float3 logx = log2(max(x, 1e-6f));
                const float3 t = (logx - shoulderStart) / shoulderStops;
                const float3 s = exp(-t);
                const float3 compressed = exp2(logHi - shoulderStops * s);
                return select(compressed, x, logx <= shoulderStart);
            }

            float3 smoothClamp(float3 color, float brightnessLimit, float rolloffStops, bool clipToLdr) {
                color = applyGamma(limitBrightness(applyGamma(color, 2.2f), brightnessLimit, rolloffStops), 1.0f / 2.2f);

                const float minVal = clipToLdr ? 0.0f : -64.0f;
                const float maxVal = clipToLdr ? 1.0f : 64.0f;
                return clamp(color, float3(minVal), float3(maxVal));
            }

            fragment float4 fragment_main(
                VertexOut vert [[stage_in]],
                texture2d<float, access::sample> image,
                sampler image_sampler,
                texture2d<float, access::sample> reference,
                sampler reference_sampler,
                texture2d<float, access::sample> colormap,
                sampler colormap_sampler,
                texture2d<float, access::sample> ditherMatrix,
                sampler ditherMatrix_sampler,
                const constant bool& hasImage,
                const constant bool& hasReference,
                const constant int& channelConfig,
                const constant float& exposure,
                const constant float& offset,
                const constant float& gamma,
                const constant float& colorMultiplier,
                const constant float& brightnessLimit,
                const constant float& rolloffStops,
                const constant bool& clipToLdr,
                const constant int& tonemap,
                const constant int& metric,
                const constant float4& channelMask,
                const constant float2& cropMin,
                const constant float2& cropMax,
                const constant float4& bgColor
            ) {
                float3 darkGray = float3(0.5f, 0.5f, 0.5f);
                float3 lightGray = float3(0.55f, 0.55f, 0.55f);
                float3 checker = int(floor(vert.checkerUv.x) + floor(vert.checkerUv.y)) % 2 == 0 ? darkGray : lightGray;

                float cropAlpha = vert.imageUv.x < cropMin.x || vert.imageUv.x > cropMax.x || vert.imageUv.y < cropMin.y || vert.imageUv.y > cropMax.y ? 0.3f : 1.0f;

                float4 val = float4(0.0f);
                if (hasImage) {
                    val = sample(image, image_sampler, channelMask, vert.imageUv, channelConfig);
                    val.a *= cropAlpha;
                }

                if (hasReference) {
                    float4 referenceVal = sample(reference, reference_sampler, channelMask, vert.referenceUv, channelConfig);
                    referenceVal.a *= cropAlpha;

                    float3 difference = val.rgb - referenceVal.rgb;
                    val = float4(applyMetric(difference, referenceVal.rgb, metric), (val.a + referenceVal.a) * 0.5f);
                }

                val += bgColor * (1.0f - val.a);
                float4 color = float4(
                    applyTonemap(
                        colorMultiplier * applyExposureAndOffset(val.rgb, exposure, offset),
                        float4(checker, 1.0f - val.a),
                        tonemap,
                        offset,
                        gamma,
                        colormap,
                        colormap_sampler
                    ),
                    1.0f
                );

                color.rgb = smoothClamp(color.rgb, brightnessLimit, rolloffStops, clipToLdr);
                return dither(color, ditherMatrix, ditherMatrix_sampler, vert.ditherUv);
            })";
#endif

        mShader = new Shader{renderPass, "ubershader", vertexShader, fragmentShader};
    } catch (const runtime_error& e) { tlog::error("Unable to compile shader: {}", e.what()); }

    // 2 Triangles
    uint32_t indices[3 * 2] = {
        0,
        1,
        2,
        2,
        3,
        0,
    };
    float positions[2 * 4] = {
        -1.f,
        -1.f,
        1.f,
        -1.f,
        1.f,
        1.f,
        -1.f,
        1.f,
    };

    mShader->set_buffer("indices", VariableType::UInt32, {3 * 2}, indices);
    mShader->set_buffer("position", VariableType::Float32, {4, 2}, positions);

    // Wrap in vector to ensure the memory is not in the app's const data segments. Hardened runtimes w/ code signing
    // would otherwise get tripped up if the texture upload happens via memory mapping, e.g. in Apple's unified archs.
    const auto fcd = vector(colormap::turbo().begin(), colormap::turbo().end());
    if (fcd.size() % 4 != 0) {
        throw runtime_error{fmt::format("Invalid false color data size. Expected multiple of 4, got {}.", fcd.size())};
    }

    mColorMap = new Texture{
        Texture::PixelFormat::RGBA, Texture::ComponentFormat::Float32, Vector2i{(int)fcd.size() / 4, 1}
    };
    mColorMap->upload(reinterpret_cast<const uint8_t*>(fcd.data()));

    mDitherMatrix = new Texture{
        Texture::PixelFormat::R,
        Texture::ComponentFormat::Float32,
        Vector2i{DITHER_MATRIX_SIZE},
        Texture::InterpolationMode::Nearest,
        Texture::InterpolationMode::Nearest,
        Texture::WrapMode::Repeat,
    };

    const auto dmat = ditherMatrix(ditherScale);
    if (dmat.size() != posProd(mDitherMatrix->size())) {
        throw runtime_error{fmt::format("Invalid dither matrix size. Expected {}, got {}.", posProd(mDitherMatrix->size()), dmat.size())};
    }

    mDitherMatrix->upload(reinterpret_cast<const uint8_t*>(dmat.data()));
}

UberShader::~UberShader() {}

void UberShader::draw(
    Vector2f pixelSize,
    Vector2f checkerSize,
    Image* image,
    const Matrix3f& transformImage,
    Image* reference,
    const Matrix3f& transformReference,
    string_view requestedChannelGroup,
    EInterpolationMode minFilter,
    EInterpolationMode magFilter,
    float exposure,
    float offset,
    float gamma,
    float colorMultiplier,
    float brightnessLimit,
    float rolloffStops,
    bool clipToLdr,
    Color backgroundColor,
    ETonemap tonemap,
    EMetric metric,
    EChannelMask channelMask,
    const optional<Box2i>& crop,
    bool splitscreenEnabled,
    float splitscreenSliderT
) {
    // We're passing the channels found in `mImage` such that, if some channels don't exist in `mReference`, they're filled with default
    // values (0 for colors, 1 for alpha).
    const auto channels = image ? image->channelsInGroup(requestedChannelGroup) : span<const string>{};
    Texture* const textureImage = image ? image->texture(channels, minFilter, magFilter) : nullptr;
    Texture* const textureReference = reference ? reference->texture(channels, minFilter, magFilter) : nullptr;

    const bool hasAlpha = channels.size() > 1 && Channel::isAlpha(channels.back()); // Only count A as alpha if it isn't the only channel.
    const int numColorChannels = channels.size() - (hasAlpha ? 1 : 0);

    EShaderChannelConfig channelConfig = EShaderChannelConfig::RGBA;
    switch (numColorChannels) {
        case 0: break; // Just rendering the checkerboard background. Value doesn't matter.
        case 1: channelConfig = hasAlpha ? EShaderChannelConfig::RA : EShaderChannelConfig::R; break;
        case 2: channelConfig = hasAlpha ? EShaderChannelConfig::RGA : EShaderChannelConfig::RG; break;
        case 3: channelConfig = hasAlpha ? EShaderChannelConfig::RGBA : EShaderChannelConfig::RGB; break;
        default: throw runtime_error{"Invalid number of color channels."};
    }

    bindCheckerboardData(pixelSize, checkerSize, backgroundColor);
    bindImageData(
        textureImage ? textureImage : mColorMap.get(), transformImage, exposure, offset, gamma, brightnessLimit, rolloffStops, tonemap
    );
    bindReferenceData(textureReference ? textureReference : mColorMap.get(), transformReference, metric);

    const Vector4f uChannelMask = {
        hasFlag(channelMask, EChannelMask::Red) ? 1.0f : 0.0f,
        hasFlag(channelMask, EChannelMask::Green) ? 1.0f : 0.0f,
        hasFlag(channelMask, EChannelMask::Blue) ? 1.0f : 0.0f,
        hasFlag(channelMask, EChannelMask::Alpha) ? 1.0f : 0.0f,
    };
    mShader->set_uniform("channelMask", uChannelMask);

    mShader->set_uniform("hasImage", (bool)textureImage);
    mShader->set_uniform("hasReference", (bool)textureReference);

    mShader->set_uniform("channelConfig", (int)channelConfig);

    mShader->set_uniform("colorMultiplier", colorMultiplier);
    mShader->set_uniform("clipToLdr", clipToLdr);

    if (crop.has_value() && textureImage) {
        mShader->set_uniform("cropMin", Vector2f{crop->min} / Vector2f{textureImage->size()});
        mShader->set_uniform("cropMax", Vector2f{crop->max} / Vector2f{textureImage->size()});
    } else {
        mShader->set_uniform("cropMin", Vector2f{-numeric_limits<float>::infinity()});
        mShader->set_uniform("cropMax", Vector2f{numeric_limits<float>::infinity()});
    }

    mShader->set_uniform("splitscreenEnabled", static_cast<float>(splitscreenEnabled));
    mShader->set_uniform("splitscreenSliderT", splitscreenSliderT);

    mShader->set_uniform("ditherSize", static_cast<float>(DITHER_MATRIX_SIZE));
    mShader->set_texture("ditherMatrix", mDitherMatrix);

    mShader->begin();
    mShader->draw_array(Shader::PrimitiveType::Triangle, 0, 6, true);
    mShader->end();
}

void UberShader::bindCheckerboardData(Vector2f pixelSize, Vector2f checkerSize, Color backgroundColor) {
    mShader->set_uniform("pixelSize", pixelSize);
    mShader->set_uniform("checkerSize", checkerSize);
    mShader->set_uniform("bgColor", backgroundColor);
}

void UberShader::bindImageData(
    Texture* textureImage,
    const Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    float brightnessLimit,
    float rolloffStops,
    ETonemap tonemap
) {
    mShader->set_texture("image", textureImage);

    mShader->set_uniform("imageScale", Vector2f{transformImage.m[0][0], transformImage.m[1][1]});
    mShader->set_uniform("imageOffset", Vector2f{transformImage.m[2][0], transformImage.m[2][1]});

    mShader->set_uniform("exposure", exposure);
    mShader->set_uniform("offset", offset);
    mShader->set_uniform("gamma", gamma);
    mShader->set_uniform("brightnessLimit", brightnessLimit);
    mShader->set_uniform("rolloffStops", rolloffStops);
    mShader->set_uniform("tonemap", static_cast<int>(tonemap));

    mShader->set_texture("colormap", mColorMap.get());
}

void UberShader::bindReferenceData(Texture* textureReference, const Matrix3f& transformReference, EMetric metric) {
    mShader->set_texture("reference", textureReference);

    mShader->set_uniform("referenceScale", Vector2f{transformReference.m[0][0], transformReference.m[1][1]});
    mShader->set_uniform("referenceOffset", Vector2f{transformReference.m[2][0], transformReference.m[2][1]});

    mShader->set_uniform("metric", static_cast<int>(metric));
}

} // namespace tev
