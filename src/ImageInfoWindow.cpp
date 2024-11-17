// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

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

ImageInfoWindow::ImageInfoWindow(Widget* parent, const std::shared_ptr<Image>& image, bool supportsHdr, function<void()> closeCallback)
    : Window{parent, "Info"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{button_panel(), "", FA_TIMES};
    closeButton->set_callback(mCloseCallback);

    set_layout(new GroupLayout{});
    set_fixed_width(400);

    TabWidget* tabWidget = new TabWidget{this};

    // Keybindings tab
    Widget* tmp = new Widget(tabWidget);
    tmp->set_fixed_height(640);
    VScrollPanel* scrollPanel = new VScrollPanel{tmp};
    tabWidget->append_tab("EXR", tmp);

    Widget* shortcuts = new Widget(scrollPanel);
    shortcuts->set_layout(new GroupLayout{});

    auto addRow = [](Widget* current, string keys, string desc) {
        auto row = new Widget{current};
        row->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Maximum, 0, 10});
        auto fieldsWidget = new Label{row, keys, "sans-bold"};
        auto descWidget = new Label{row, desc, "sans"};
        descWidget->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Maximum});
        descWidget->set_fixed_width(250);
    };

    new Label{shortcuts, "Attributes", "sans-bold", 18};
    auto imageLoading = new Widget{shortcuts};
    imageLoading->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    for (const auto& it:image->attributes()) {
        addRow(imageLoading, it.first, it.second);
    }

    // Make the keybindings page as big as is needed to fit the about tab
    perform_layout(screen()->nvg_context());
    // scrollPanel->set_fixed_height(about->height() + 12);
    scrollPanel->set_fixed_height(640-12);
    tabWidget->set_fixed_height(640);
    
    tabWidget->set_selected_id(0);
    tabWidget->set_callback([tabWidget] (int id) mutable {
        tabWidget->set_selected_id(id);
    });
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

}
