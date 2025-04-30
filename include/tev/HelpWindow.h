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

#pragma once

#include <tev/Common.h>

#include <nanogui/window.h>

#include <string>

namespace tev {

class Ipc;

class HelpWindow : public nanogui::Window {
public:
    HelpWindow(nanogui::Widget* parent, bool supportsHdr, const Ipc& ipc, std::function<void()> closeCallback);

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    static std::string COMMAND;
    static std::string ALT;

private:
    std::function<void()> mCloseCallback;
};

} // namespace tev
