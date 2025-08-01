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

#include <filesystem>

struct GLFWwindow;

namespace tev {

class FileDialog {
public:
    FileDialog(GLFWwindow* glfwWindow);
    ~FileDialog();

    // Disable copy and move semantics
    FileDialog(const FileDialog&) = delete;
    FileDialog& operator=(const FileDialog&) = delete;
    FileDialog(FileDialog&&) = delete;
    FileDialog& operator=(FileDialog&&) = delete;

    std::vector<fs::path> openFileDialog(const std::vector<std::pair<std::string, std::string>>& filters, const fs::path& defaultPath = {});
    fs::path saveFileDialog(const std::vector<std::pair<std::string, std::string>>& filters, const fs::path& defaultPath = {});

private:
    GLFWwindow* mGlfwWindow;
};

} // namespace tev
