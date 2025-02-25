/*
 * tev -- the EXR viewer
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

#include <nanogui/shader.h>
#include <nanogui/texture.h>
#include <nanogui/vector.h>

#include <optional>

namespace tev {

class UberShader {
public:
    UberShader(nanogui::RenderPass* renderPass);
    virtual ~UberShader();

    // Draws just a checkerboard
    void draw(const nanogui::Vector2f& pixelSize, const nanogui::Vector2f& checkerSize);

    // Draws an image
    void draw(
        const nanogui::Vector2f& pixelSize,
        const nanogui::Vector2f& checkerSize,
        nanogui::Texture* textureImage,
        const nanogui::Matrix3f& transformImage,
        float exposure,
        float offset,
        float gamma,
        bool clipToLdr,
        ETonemap tonemap,
        const std::optional<Box2i>& crop
    );

    // Draws a difference between a reference and an image
    void draw(
        const nanogui::Vector2f& pixelSize,
        const nanogui::Vector2f& checkerSize,
        nanogui::Texture* textureImage,
        const nanogui::Matrix3f& transformImage,
        nanogui::Texture* textureReference,
        const nanogui::Matrix3f& transformReference,
        float exposure,
        float offset,
        float gamma,
        bool clipToLdr,
        ETonemap tonemap,
        EMetric metric,
        const std::optional<Box2i>& crop
    );

    const nanogui::Color& backgroundColor() { return mBackgroundColor; }

    void setBackgroundColor(const nanogui::Color& color) { mBackgroundColor = color; }

private:
    void bindCheckerboardData(const nanogui::Vector2f& pixelSize, const nanogui::Vector2f& checkerSize);

    void bindImageData(
        nanogui::Texture* textureImage, const nanogui::Matrix3f& transformImage, float exposure, float offset, float gamma, ETonemap tonemap
    );

    void bindReferenceData(nanogui::Texture* textureReference, const nanogui::Matrix3f& transformReference, EMetric metric);

    nanogui::ref<nanogui::Shader> mShader;
    nanogui::ref<nanogui::Texture> mColorMap;

    nanogui::Color mBackgroundColor = nanogui::Color(0, 0, 0, 0);
};

} // namespace tev
