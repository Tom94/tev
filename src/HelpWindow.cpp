// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/HelpWindow.h"

#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/window.h>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

HelpWindow::HelpWindow(Widget *parent)
    : Window{parent, "Help – Keybindings"} {
    setLayout(new GroupLayout{});

    setFixedWidth(400);
    auto addRow = [](Widget* current, string keys, string desc) {
        auto row = new Widget{current};
        row->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 0, 10});
        auto descWidget = new Label{row, desc, "sans"};
        descWidget->setFixedWidth(210);
        new Label{row, keys, "sans-bold"};
    };

#ifdef __APPLE__
    string command = "Cmd";
#else
    string command = "Ctrl";
#endif

#ifdef __APPLE__
    string alt = "Opt";
#else
    string alt = "Alt";
#endif

    new Label{this, "Image Selection", "sans-bold", 18};
    auto imageSelection = new Widget{this};
    imageSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 5});

    addRow(imageSelection, "1…9",       "Select N-th Image");
    addRow(imageSelection, "Down or W", "Select Next Image");
    addRow(imageSelection, "Up or W",   "Select Previous Image");
    
    new Label{this, "Reference Selection", "sans-bold", 18};
    auto referenceSelection = new Widget{this};
    referenceSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 5});

    addRow(referenceSelection, "Shift+1…9",             "Select N-th Image as Reference");
    addRow(referenceSelection, "Shift+Down or Shift+W", "Select Next Image as Reference");
    addRow(referenceSelection, "Shift+Up or Shift+W",   "Select Previous Image as Reference");

    new Label{this, "Layer Selection", "sans-bold", 18};
    auto layerSelection = new Widget{this};
    layerSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 5});

    addRow(layerSelection, "Ctrl+1…9",   "Select N-th Layer");
    addRow(layerSelection, "Right or D", "Select Next Layer");
    addRow(layerSelection, "Left or A",  "Select Previous Layer");

    new Label{this, "Hotkeys", "sans-bold", 18};
    auto hotkeys = new Widget{this};
    hotkeys->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 5});

    addRow(hotkeys, alt + "+Enter",       "Maximize");
    addRow(hotkeys, command + "+B",       "Toggle GUI");
    addRow(hotkeys, "E",                  "Increase Exposure by 0.5");
    addRow(hotkeys, "Shift+E",            "Decrease Exposure by 0.5");
    addRow(hotkeys, "F",                  "Fit Image to Screen");
    addRow(hotkeys, "H",                  "Show Help (this Window)");
    addRow(hotkeys, "N",                  "Normalize Image to [0, 1]");
    addRow(hotkeys, "O",                  "Increase Offset by 0.1");
    addRow(hotkeys, "Shift+O",            "Decrease Offset by 0.1");
    addRow(hotkeys, command + "+P",       "Find Image or Layer");
    addRow(hotkeys, "Q or Esc",           "Quit");
    addRow(hotkeys, "R",                  "Reset Image Parameters");
    addRow(hotkeys, command + "+R or F5", "Reload Image");
}

TEV_NAMESPACE_END
