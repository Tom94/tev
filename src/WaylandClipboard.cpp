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

#include <string_view>

using namespace std;

namespace tev {

void waylandSetClipboardPngImage(string_view data) {
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        throw runtime_error("Wayland clipboard operations are only supported on Wayland.");
    }

#if !defined(__APPLE__) && !defined(_WIN32)
    glfwSetWaylandClipboardData(data.data(), "image/png", data.size());
#endif
}

string_view waylandGetClipboardPngImage() {
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        throw runtime_error("Wayland clipboard operations are only supported on Wayland.");
    }

#if !defined(__APPLE__) && !defined(_WIN32)
    size_t size = 0;
    const char* data = glfwGetWaylandClipboardData("image/png", &size);
    return {data, size};
#else
    return {};
#endif
}

} // namespace tev
