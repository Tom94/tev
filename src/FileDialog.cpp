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
#include <tev/FileDialog.h>

#if defined(__APPLE__)
#    define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#    define GLFW_EXPOSE_NATIVE_WIN32
#else
#    define GLFW_EXPOSE_NATIVE_WAYLAND
#    define GLFW_EXPOSE_NATIVE_X11
#endif

#include <nfd_glfw3.h>

using namespace std;

namespace tev {

FileDialog::FileDialog(GLFWwindow* glfwWindow) : mGlfwWindow{glfwWindow} {
    if (auto result = NFD_Init(); result != NFD_OKAY) {
        throw runtime_error(fmt::format("Failed to initialize Native File Dialog: {}", NFD_GetError()));
    }
}

FileDialog::~FileDialog() { NFD_Quit(); }

vector<fs::path> FileDialog::openFileDialog(const vector<pair<string, string>>& filters, const fs::path& defaultPath) {
    vector<nfdu8filteritem_t> nfdFilters;
    for (const auto& filter : filters) {
        nfdFilters.push_back({filter.first.c_str(), filter.second.c_str()});
    }

    nfdopendialogu8args_t args = {};
    args.filterList = nfdFilters.data();
    args.filterCount = nfdFilters.size();

    if (!NFD_GetNativeWindowFromGLFWWindow(mGlfwWindow, &args.parentWindow)) {
        tlog::debug() << "Failed to get native window from GLFW window.";
    }

    const nfdpathset_t* outPaths = nullptr;
    nfdresult_t result = NFD_OpenDialogMultipleU8_With(&outPaths, &args);

    auto pathSetGuard = ScopeGuard{[&outPaths]() {
        if (outPaths) {
            NFD_PathSet_Free(outPaths);
        }
    }};

    if (result == NFD_CANCEL) {
        return {};
    } else if (result != NFD_OKAY) {
        throw runtime_error(fmt::format("Open dialog error: {}", NFD_GetError()));
    }

    nfdpathsetenum_t enumerator = {};
    if (!NFD_PathSet_GetEnum(outPaths, &enumerator)) {
        throw runtime_error(fmt::format("Failed to get path set enumerator: {}", NFD_GetError()));
    }

    auto pathEnumGuard = ScopeGuard{[&enumerator]() { NFD_PathSet_FreeEnum(&enumerator); }};

    vector<fs::path> paths;

    nfdchar_t* outPath = nullptr;
    while (NFD_PathSet_EnumNextU8(&enumerator, &outPath) && outPath) {
        auto pathGuard = ScopeGuard{[&outPath]() {
            if (outPath) {
                NFD_FreePathU8(outPath);
            }
        }};

        paths.emplace_back(toPath(outPath));
    }

    return paths;
}

fs::path FileDialog::saveFileDialog(const std::vector<std::pair<std::string, std::string>>& filters, const fs::path& defaultPath) {
    vector<nfdu8filteritem_t> nfdFilters;
    for (const auto& filter : filters) {
        nfdFilters.push_back({filter.first.c_str(), filter.second.c_str()});
    }

    nfdsavedialogu8args_t args = {};
    args.filterList = nfdFilters.data();
    args.filterCount = nfdFilters.size();

    // const auto imageName = string{mCurrentImage->name()};
    // args.defaultName = imageName.c_str();

    if (!NFD_GetNativeWindowFromGLFWWindow(mGlfwWindow, &args.parentWindow)) {
        tlog::debug() << "Failed to get native window from GLFW window.";
    }

    nfdu8char_t* outPath = nullptr;
    nfdresult_t result = NFD_SaveDialogU8_With(&outPath, &args);

    auto pathGuard = ScopeGuard{[&outPath]() {
        if (outPath) {
            NFD_FreePathU8(outPath);
        }
    }};

    if (result == NFD_CANCEL) {
        return {};
    } else if (result != NFD_OKAY) {
        throw runtime_error(fmt::format("Save dialog error: {}", NFD_GetError()));
    }

    return toPath(outPath);
}

} // namespace tev
