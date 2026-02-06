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

#include <tev/Common.h>
#include <tev/WaylandClipboard.h>

#if !defined(__APPLE__) && !defined(_WIN32)
#    define GLFW_EXPOSE_NATIVE_WAYLAND
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

using namespace std;

namespace tev {

void waylandSetClipboardPngImage(const char* data, size_t size) {
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        throw runtime_error("Wayland clipboard operations are only supported on Wayland.");
    }

#if !defined(__APPLE__) && !defined(_WIN32)
    glfwSetWaylandClipboardData(data, "image/png", size);
#endif
}

const char* waylandGetClipboardPngImage(size_t* size) {
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        throw runtime_error("Wayland clipboard operations are only supported on Wayland.");
    }

#if !defined(__APPLE__) && !defined(_WIN32)
    return glfwGetWaylandClipboardData("image/png", size);
#else
    *size = 0;
    return nullptr;
#endif
}

} // namespace tev
