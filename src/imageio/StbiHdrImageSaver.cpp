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

#include <tev/imageio/StbiHdrImageSaver.h>

#include <stb_image_write.h>

#include <ostream>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

void StbiHdrImageSaver::save(ostream& oStream, const fs::path&, span<const float> data, const Vector2i& imageSize, int nChannels) const {
    static const auto stbiOStreamWrite = [](void* context, void* stbidata, int size) {
        reinterpret_cast<ostream*>(context)->write(reinterpret_cast<char*>(stbidata), size);
    };

    stbi_write_hdr_to_func(stbiOStreamWrite, &oStream, imageSize.x(), imageSize.y(), nChannels, data.data());
}

} // namespace tev
