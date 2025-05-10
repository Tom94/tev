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

#include <tev/ImageInfoWindow.h>

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/tabwidget.h>
#include <nanogui/vscrollpanel.h>
#include <nanogui/window.h>

using namespace nanogui;
using namespace std;

namespace tev {

void addRows(Widget* current, const AttributeNode& node, int indentation) {
    auto row = new Widget{current};
    row->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Maximum, 0, 0});

    auto spacer = new Widget{row};
    spacer->set_fixed_width(indentation * 8);

    auto nameWidget = new Label{row, node.name, "sans-bold"};
    nameWidget->set_fixed_width(160 - indentation * 8);

    auto valueWidget = new Label{row, node.value, "sans"};
    valueWidget->set_fixed_width(320);

    auto typeWidget = new Label{row, node.type, "sans"};
    typeWidget->set_fixed_width(160);

    for (const auto& it : node.children) {
        addRows(current, it, indentation + 1);
    }
};

ImageInfoWindow::ImageInfoWindow(Widget* parent, const std::shared_ptr<Image>& image, bool supportsHdr, function<void()> closeCallback) :
    Window{parent, "Info"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{button_panel(), "", FA_TIMES};
    closeButton->set_callback(mCloseCallback);

    static const int WINDOW_WIDTH = 700;
    static const int WINDOW_HEIGHT = 680;

    set_layout(new GroupLayout{});
    set_fixed_width(WINDOW_WIDTH);

    mTabWidget = new TabWidget{this};
    mTabWidget->set_fixed_height(WINDOW_HEIGHT - 12);

    // Each attributes entry is a tab
    for (const auto& tab : image->attributes()) {
        Widget* tmp = new Widget(mTabWidget);

        VScrollPanel* scrollPanel = new VScrollPanel{tmp};
        scrollPanel->set_fixed_height(WINDOW_HEIGHT - 40);
        scrollPanel->set_fixed_width(WINDOW_WIDTH - 40);

        mTabWidget->append_tab(tab.name, tmp);

        Widget* container = new Widget(scrollPanel);
        container->set_layout(new GroupLayout{});

        // Each top-level child of the attribute is a section
        for (const auto& section : tab.children) {
            new Label{container, section.name, "sans-bold", 18};
            auto attributes = new Widget{container};
            attributes->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

            for (const auto& c : section.children) {
                addRows(attributes, c, 0);
            }
        }
    }

    perform_layout(screen()->nvg_context());

    mTabWidget->set_callback([this](int id) mutable { mTabWidget->set_selected_id(id); });
    if (mTabWidget->tab_count() > 0) {
        mTabWidget->set_selected_id(0);
    }
}

bool ImageInfoWindow::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Window::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            mCloseCallback();
            return true;
        } else if (key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                mTabWidget->set_selected_id((mTabWidget->selected_id() - 1 + mTabWidget->tab_count()) % mTabWidget->tab_count());
            } else {
                mTabWidget->set_selected_id((mTabWidget->selected_id() + 1) % mTabWidget->tab_count());
            }

            return true;
        }
    }

    return false;
}

} // namespace tev
