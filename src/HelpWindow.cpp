// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/HelpWindow.h"

#include <nanogui/button.h>
#include <nanogui/entypo.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/window.h>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

#ifdef __APPLE__
string HelpWindow::COMMAND = "Cmd";
#else
string HelpWindow::COMMAND = "Ctrl";
#endif

#ifdef __APPLE__
string HelpWindow::ALT = "Opt";
#else
string HelpWindow::ALT = "Alt";
#endif

HelpWindow::HelpWindow(Widget *parent, function<void()> closeCallback)
    : Window{parent, "Help – Keybindings"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{buttonPanel(), "", ENTYPO_ICON_CROSS};
    closeButton->setCallback(mCloseCallback);

    setLayout(new GroupLayout{});

    setFixedWidth(400);
    auto addRow = [](Widget* current, string keys, string desc) {
        auto row = new Widget{current};
        row->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 0, 10});
        auto descWidget = new Label{row, desc, "sans"};
        descWidget->setFixedWidth(210);
        new Label{row, keys, "sans-bold"};
    };

    auto addSpacer = [](Widget* current) {
        auto row = new Widget{current};
        row->setHeight(10);
    };

    new Label{this, "Image Loading", "sans-bold", 18};
    auto imageLoading = new Widget{this};
    imageLoading->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(imageLoading, COMMAND + "+O", "Open Image");
    addRow(imageLoading, COMMAND + "+R or F5", "Reload Image");
    addRow(imageLoading, COMMAND + "+Shift+R or "s + COMMAND + "+F5", "Reload All Images");
    addRow(imageLoading, COMMAND + "+W", "Close Image");

    new Label{this, "Image Options", "sans-bold", 18};
    auto imageSelection = new Widget{this};
    imageSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(imageSelection, "1…9",       "Select N-th Image");
    addRow(imageSelection, "Down or W", "Select Next Image");
    addRow(imageSelection, "Up or W",   "Select Previous Image");

    addSpacer(imageSelection);

    addRow(imageSelection, "F", "Fit Image to Screen");
    addRow(imageSelection, "N", "Normalize Image to [0, 1]");
    addRow(imageSelection, "R", "Reset Image Parameters");

    addSpacer(imageSelection);

    addRow(imageSelection, "Shift+Right or Shift+D", "Select Next Tonemap");
    addRow(imageSelection, "Shift+Left or Shift+A", "Select Previous Tonemap");

    addSpacer(imageSelection);

    addRow(imageSelection, "E", "Increase Exposure by 0.5");
    addRow(imageSelection, "Shift+E", "Decrease Exposure by 0.5");
    addRow(imageSelection, "O", "Increase Offset by 0.1");
    addRow(imageSelection, "Shift+O", "Decrease Offset by 0.1");

    new Label{this, "Reference Options", "sans-bold", 18};
    auto referenceSelection = new Widget{this};
    referenceSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(referenceSelection, "Shift+1…9",             "Select N-th Image as Reference");
    addRow(referenceSelection, "Shift+Down or Shift+W", "Select Next Image as Reference");
    addRow(referenceSelection, "Shift+Up or Shift+W",   "Select Previous Image as Reference");

    addSpacer(referenceSelection);

    addRow(referenceSelection, "Ctrl+Right or Ctrl+D", "Select Next Error Metric");
    addRow(referenceSelection, "Ctrl+Left or Ctrl+A", "Select Previous Error Metric");

    new Label{this, "Layer Options", "sans-bold", 18};
    auto layerSelection = new Widget{this};
    layerSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(layerSelection, "Ctrl+1…9",   "Select N-th Layer");
    addRow(layerSelection, "Right or D", "Select Next Layer");
    addRow(layerSelection, "Left or A",  "Select Previous Layer");

    new Label{this, "Interface", "sans-bold", 18};
    auto interface = new Widget{this};
    interface->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(interface, ALT + "+Enter", "Maximize");
    addRow(interface, COMMAND + "+B", "Toggle GUI");
    addRow(interface, "H", "Show Help (this Window)");
    addRow(interface, COMMAND + "+P", "Find Image or Layer");
    addRow(interface, "Q or Esc", "Quit");
}

bool HelpWindow::keyboardEvent(int key, int scancode, int action, int modifiers) {
    if (Window::keyboardEvent(key, scancode, action, modifiers)) {
        return true;
    }

    if (key == GLFW_KEY_ESCAPE) {
        mCloseCallback();
        return true;
    }

    return false;
}

TEV_NAMESPACE_END
