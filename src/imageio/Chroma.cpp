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

#include <tev/Common.h>
#include <tev/imageio/Chroma.h>

#include <nanogui/vector.h>

#include <ImfChromaticities.h>

using namespace std;
using namespace nanogui;

namespace tev {

nanogui::Matrix4f convertChromaToRec709(std::array<nanogui::Vector2f, 4> chroma) {
    Imf::Chromaticities rec709; // default rec709 (sRGB) primaries
    Imf::Chromaticities imfChroma = {
        {chroma[0].x(), chroma[0].y()},
        {chroma[1].x(), chroma[1].y()},
        {chroma[2].x(), chroma[2].y()},
        {chroma[3].x(), chroma[3].y()},
    };

    // equality comparison for Imf::Chromaticities instances
    auto chromaEq = [](const Imf::Chromaticities& a, const Imf::Chromaticities& b) {
        return (a.red - b.red).length2() + (a.green - b.green).length2() + (a.blue - b.blue).length2() + (a.white - b.white).length2() <
            1e-6f;
    };

    if (chromaEq(imfChroma, rec709)) {
        return nanogui::Matrix4f{1.0f};
    }

    Imath::M44f M = Imf::RGBtoXYZ(imfChroma, 1) * Imf::XYZtoRGB(rec709, 1);

    nanogui::Matrix4f toRec709;
    for (int m = 0; m < 4; ++m) {
        for (int n = 0; n < 4; ++n) {
            toRec709.m[m][n] = M.x[m][n];
        }
    }

    return toRec709;
}

} // namespace tev
