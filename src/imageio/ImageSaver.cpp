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

#include <tev/imageio/ImageSaver.h>

#include <tev/imageio/ExrImageSaver.h>
#include <tev/imageio/QoiImageSaver.h>
#include <tev/imageio/StbiHdrImageSaver.h>
#include <tev/imageio/StbiLdrImageSaver.h>

#ifdef TEV_SUPPORT_JXL
#include <tev/imageio/JxlImageSaver.h>
#endif

#include <vector>

using namespace std;

namespace tev {

const vector<unique_ptr<ImageSaver>>& ImageSaver::getSavers() {
    auto makeSavers = [] {
        vector<unique_ptr<ImageSaver>> imageSavers;
        imageSavers.emplace_back(new ExrImageSaver());
        imageSavers.emplace_back(new QoiImageSaver());
#ifdef TEV_SUPPORT_JXL
        imageSavers.emplace_back(new JxlImageSaver());
#endif
        imageSavers.emplace_back(new StbiHdrImageSaver());
        imageSavers.emplace_back(new StbiLdrImageSaver());
        return imageSavers;
    };

    static const vector imageSavers = makeSavers();
    return imageSavers;
}

} // namespace tev
