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
#include <tev/Task.h>

#include <nanogui/vector.h>

class AppleMakerNote;

namespace tev {

// Applies an Apple gain map to the given image. Both the image and the gainmap are expected to be in linear space, have RGBA interleaved
// layout, and have the same size.
Task<void> applyAppleGainMap(float* __restrict image, const float* __restrict gainMap, const nanogui::Vector2i& size, int priority, const AppleMakerNote* amn);

}


