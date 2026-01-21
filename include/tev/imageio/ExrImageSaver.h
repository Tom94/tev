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

#include <tev/Common.h>
#include <tev/imageio/ImageSaver.h>

#include <ostream>
#include <string_view>

namespace tev {

class ExrImageSaver : public TypedImageSaver<float> {
public:
    Task<void> save(
        std::ostream& oStream, const fs::path& path, std::span<const float> data, const nanogui::Vector2i& imageSize, int nChannels
    ) const override;

    EAlphaKind alphaKind(std::string_view) const override { return EAlphaKind::Premultiplied; }

    virtual bool canSaveFile(std::string_view extension) const override { return toLower(extension) == ".exr"; }
};

} // namespace tev
