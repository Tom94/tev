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

#include <nanogui/vector.h>

#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace tev {

template <typename T> class TypedImageSaver;

class ImageSaver {
public:
    virtual ~ImageSaver() {}

    virtual EAlphaKind alphaKind(std::string_view extension) const = 0;
    EAlphaKind alphaKind(const fs::path& path) const { return alphaKind(std::string_view{toLower(toString(path.extension()))}); }

    virtual bool canSaveFile(std::string_view extension) const = 0;
    bool canSaveFile(const fs::path& path) const { return canSaveFile(std::string_view{toLower(toString(path.extension()))}); }

    static const std::vector<std::unique_ptr<ImageSaver>>& getSavers();
};

template <typename T> class TypedImageSaver : public ImageSaver {
public:
    virtual void save(
        std::ostream& oStream, const fs::path& path, std::span<const T> data, const nanogui::Vector2i& imageSize, int nChannels
    ) const = 0;
};

} // namespace tev
