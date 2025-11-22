/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <tev/Box.h>
#include <tev/Image.h>

#include <nanogui/shader.h>
#include <nanogui/texture.h>
#include <nanogui/vector.h>

#include <optional>
#include <string_view>

namespace tev {

class UberShader {
public:
    UberShader(nanogui::RenderPass* renderPass, float ditherScale);
    virtual ~UberShader();

    void draw(
        const nanogui::Vector2f& pixelSize,
        const nanogui::Vector2f& checkerSize,
        Image* textureImage,
        const nanogui::Matrix3f& transformImage,
        Image* textureReference,
        const nanogui::Matrix3f& transformReference,
        std::string_view requestedChannelGroup,
        EInterpolationMode minFilter,
        EInterpolationMode magFilter,
        float exposure,
        float offset,
        float gamma,
        float colorMultiplier,
        bool clipToLdr,
        const nanogui::Color& backgroundColor,
        ETonemap tonemap,
        EMetric metric,
        const std::optional<Box2i>& crop
    );

private:
    void bindCheckerboardData(const nanogui::Vector2f& pixelSize, const nanogui::Vector2f& checkerSize, const nanogui::Color& backgroundColor);

    void bindImageData(
        nanogui::Texture* textureImage, const nanogui::Matrix3f& transformImage, float exposure, float offset, float gamma, ETonemap tonemap
    );

    void bindReferenceData(nanogui::Texture* textureReference, const nanogui::Matrix3f& transformReference, EMetric metric);

    nanogui::ref<nanogui::Shader> mShader;
    nanogui::ref<nanogui::Texture> mColorMap;
    nanogui::ref<nanogui::Texture> mDitherMatrix;
};

} // namespace tev
