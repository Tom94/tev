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

#include <sstream>
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

#ifdef __APPLE__
string ImageInfoWindow::COMMAND = "Cmd";
#else
string ImageInfoWindow::COMMAND = "Ctrl";
#endif

#ifdef __APPLE__
string ImageInfoWindow::ALT = "Opt";
#else
string ImageInfoWindow::ALT = "Alt";
#endif

void addRow(Widget* current, const string& name, const string& type, const string& value, int indentation) {
    auto row = new Widget{current};
    row->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Maximum, 0, 0});
    std::ostringstream oss;
    for (int i = 0; i < indentation; ++i) {
        oss << "-";
    }
    if (indentation > 0) {
        oss << "> ";
    }
    oss << name;
    auto nameWidget = new Label{row, oss.str(), "sans-bold"};
    nameWidget->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Maximum});
    nameWidget->set_fixed_width(150);
    auto valueWidget = new Label{row, value, "sans"};
    valueWidget->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Maximum});
    valueWidget->set_fixed_width(150);
    auto typeWidget = new Label{row, type, "sans"};
    typeWidget->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Maximum});
    typeWidget->set_fixed_width(150);
};

ImageInfoWindow::ImageInfoWindow(Widget* parent, const std::shared_ptr<Image>& image, bool supportsHdr, function<void()> closeCallback) :
    Window{parent, "Info"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{button_panel(), "", FA_TIMES};
    closeButton->set_callback(mCloseCallback);

    set_layout(new GroupLayout{});
    set_fixed_width(550);

    TabWidget* tabWidget = new TabWidget{this};

    // Keybindings tab
    Widget* tmp = new Widget(tabWidget);
    tmp->set_fixed_height(640);
    VScrollPanel* scrollPanel = new VScrollPanel{tmp};
    tabWidget->append_tab("EXR", tmp);

    Widget* shortcuts = new Widget(scrollPanel);
    shortcuts->set_layout(new GroupLayout{});

    new Label{shortcuts, "Attributes", "sans-bold", 18};
    auto imageLoading = new Widget{shortcuts};
    imageLoading->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(imageLoading, "name", "type", "value", 0);
    for (const auto& it : (image->attributes().children)) {
        addRow(imageLoading, it.name, it.type, it.value, 0);
        if (it.children.size() > 0) {
            for (const auto& itChild : (it.children)) {
                addRow(imageLoading, itChild.name, itChild.type, itChild.value, 1);
            }
        }
    }

    // Make the keybindings page as big as is needed to fit the about tab
    perform_layout(screen()->nvg_context());
    // scrollPanel->set_fixed_height(about->height() + 12);
    scrollPanel->set_fixed_height(640 - 12);
    tabWidget->set_fixed_height(640);

    tabWidget->set_selected_id(0);
    tabWidget->set_callback([tabWidget](int id) mutable { tabWidget->set_selected_id(id); });
}

bool ImageInfoWindow::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Window::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    if (key == GLFW_KEY_ESCAPE) {
        mCloseCallback();
        return true;
    }

    return false;
}

} // namespace tev
