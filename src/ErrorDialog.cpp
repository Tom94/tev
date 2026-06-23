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

#include <tev/ErrorDialog.h>

#include <nanogui/label.h>
#include <nanogui/messagedialog.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>

using namespace nanogui;
using namespace std;

namespace tev {

ErrorDialog::ErrorDialog(
    nanogui::Widget* parent,
    Type type,
    string_view title,
    string_view message,
    string_view button_text,
    string_view alt_button_text,
    bool alt_button,
    int max_width,
    int max_height
) :
    MessageDialog(parent, type, title, message, button_text, alt_button_text, alt_button, max_width, max_height) {}

bool ErrorDialog::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (MessageDialog::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    if (action == GLFW_PRESS && (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER)) {
        dispose();
        return true;
    }

    return false;
}

} // namespace tev
