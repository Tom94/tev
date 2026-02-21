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
#include <tev/Image.h>
#include <tev/imageio/IsoGainMapMetadata.h>

#include <optional>
#include <string_view>

namespace tev {

class Xmp {
public:
    Xmp(std::string_view xmpData);

    const AttributeNode& attributes() const { return mAttributes; }
    EOrientation orientation() const { return mOrientation; }
    const std::optional<IsoGainMapMetadata>& isoGainMapMetadata() const { return mIsoGainMapMetadata; }
    std::string_view appleAuxImgType() const { return mAppleAuxImgType; }

private:
    AttributeNode mAttributes;
    EOrientation mOrientation = EOrientation::None;
    std::optional<IsoGainMapMetadata> mIsoGainMapMetadata;
    std::string mAppleAuxImgType = "";
};

} // namespace tev
