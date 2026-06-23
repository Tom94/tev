/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
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

#include <nanogui/messagedialog.h>
#include <nanogui/widget.h>

#include <string_view>

namespace tev {

class ErrorDialog final : public nanogui::MessageDialog {
public:
    ErrorDialog(
        nanogui::Widget* parent,
        Type type,
        std::string_view title = "Untitled",
        std::string_view message = "Message",
        std::string_view button_text = "OK",
        std::string_view alt_button_text = "Cancel",
        bool alt_button = false,
        int max_width = 600,
        int max_height = 500
    );

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;
};

} // namespace tev
