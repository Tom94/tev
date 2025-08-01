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

#include <tev/imageio/ImageSaver.h>

#include <ostream>

namespace tev {

class StbiLdrImageSaver : public TypedImageSaver<char> {
public:
    void save(
        std::ostream& oStream, const fs::path& path, std::span<const char> data, const nanogui::Vector2i& imageSize, int nChannels
    ) const override;

    bool hasPremultipliedAlpha() const override { return false; }

    virtual bool canSaveFile(std::string_view extension) const override {
        std::string lowerExtension = toLower(extension);
        return lowerExtension == ".jpg" || lowerExtension == ".jpeg" || lowerExtension == ".png" || lowerExtension == ".bmp" ||
            lowerExtension == ".tga";
    }
};

} // namespace tev
