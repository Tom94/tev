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

#include <tev/HelpWindow.h>

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
string HelpWindow::COMMAND = "Cmd";
#else
string HelpWindow::COMMAND = "Ctrl";
#endif

#ifdef __APPLE__
string HelpWindow::ALT = "Opt";
#else
string HelpWindow::ALT = "Alt";
#endif

HelpWindow::HelpWindow(Widget* parent, bool supportsHdr, function<void()> closeCallback) :
    Window{parent, "Help"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{button_panel(), "", FA_TIMES};
    closeButton->set_callback(mCloseCallback);

    set_layout(new GroupLayout{});
    set_fixed_width(640);

    TabWidget* tabWidget = new TabWidget{this};

    // Keybindings tab
    Widget* tmp = new Widget(tabWidget);
    // tmp->set_fixed_height(400);
    VScrollPanel* scrollPanel = new VScrollPanel{tmp};
    tabWidget->append_tab("Keybindings", tmp);

    Widget* shortcuts = new Widget(scrollPanel);
    shortcuts->set_layout(new GroupLayout{});

    auto addRow = [](Widget* current, string keys, string desc) {
        auto row = new Widget{current};
        row->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 0, 10});
        auto descWidget = new Label{row, desc, "sans"};
        descWidget->set_fixed_width(250);
        new Label{row, keys, "sans-bold"};
    };

    new Label{shortcuts, "Image loading", "sans-bold", 18};
    auto imageLoading = new Widget{shortcuts};
    imageLoading->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(imageLoading, COMMAND + "+O", "Open image");
    addRow(imageLoading, COMMAND + "+S", "Save view as image");
    addRow(imageLoading, COMMAND + "+R or F5", "Reload image");
    addRow(imageLoading, COMMAND + "+Shift+R or " + COMMAND + "+F5", "Reload all images");
    addRow(imageLoading, COMMAND + "+W", "Close image");
    addRow(imageLoading, COMMAND + "+Shift+W", "Close all images");
    addRow(imageLoading, COMMAND + "+C", "Copy image to clipboard");
    addRow(imageLoading, COMMAND + "+Shift+C", "Copy image's path to clipboard");
    addRow(imageLoading, COMMAND + "+V", "Paste image from clipboard");

    new Label{shortcuts, "Image options", "sans-bold", 18};
    auto imageSelection = new Widget{shortcuts};
    imageSelection->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(imageSelection, "Left Click", "Select hovered image");
    addRow(imageSelection, "1…9", "Select N-th image");
    addRow(imageSelection, "Down/Up or S/W or Ctrl+Tab/Ctrl+Shift+Tab", "Select next/previous image");
    addRow(imageSelection, "Home/End", "Select first/last image");
    addRow(imageSelection, "Space", "Toggle playback of images as video");

    addRow(imageSelection, "Click & Drag or H/J/K/L (+Shift/Ctrl)", "Translate image");
    addRow(imageSelection, "Click & Drag+C (hold)", "Crop image");
    addRow(imageSelection, "+/- or Scroll (+Shift/Ctrl)", "Zoom in/out of image");

    addRow(imageSelection, COMMAND + "+0", "Zoom to actual size");
    addRow(imageSelection, COMMAND + "+9/F", "Zoom to fit");
    addRow(imageSelection, "N", "Normalize image to [0, 1]");
    addRow(imageSelection, "R", "Reset image parameters");
    if (supportsHdr) {
        addRow(imageSelection, "L", "Display the image as if on an LDR screen");
    }

    addRow(imageSelection, "Shift+Right/Left or Shift+D/A", "Select next/previous tonemap");

    addRow(imageSelection, "E/Shift+E", "Increase/decrease exposure by 0.5");
    addRow(imageSelection, "O/Shift+O", "Increase/decrease offset by 0.1");

    addRow(imageSelection, "B (hold)", "Draw a border around the image");
    addRow(imageSelection, "Shift+Ctrl (hold)", "Display raw bytes on pixels when zoomed-in");
#ifdef __APPLE__
    addRow(imageSelection, "Enter", "Rename the image");
#else
    addRow(imageSelection, "F2", "Rename the image");
#endif

    new Label{shortcuts, "Reference options", "sans-bold", 18};
    auto referenceSelection = new Widget{shortcuts};
    referenceSelection->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(referenceSelection, "Shift (hold)", "View currently selected reference");
    addRow(referenceSelection, "Shift+Left Click or Right Click", "Select hovered image as reference");
    addRow(referenceSelection, "Shift+1…9", "Select N-th image as reference");
    addRow(referenceSelection, "Shift+Down/Up or Shift+S/W", "Select next/previous image as reference");

    addRow(referenceSelection, "Ctrl (hold)", "View selected image if reference is selected");
    addRow(referenceSelection, "Ctrl+Right/Left or Ctrl+D/A", "Select next/previous error metric");

    new Label{shortcuts, "Channel group options", "sans-bold", 18};
    auto groupSelection = new Widget{shortcuts};
    groupSelection->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(groupSelection, "Left Click", "Select hovered channel group");
    addRow(groupSelection, "Ctrl+1…9", "Select N-th channel group");
    addRow(groupSelection, "Right/Left or D/A or ]/[", "Select next/previous channel group");
    addRow(groupSelection, "X", "Explode current channel group");

    new Label{shortcuts, "Interface", "sans-bold", 18};
    auto ui = new Widget{shortcuts};
    ui->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(ui, ALT + "+Enter", "Maximize");
    addRow(ui, COMMAND + "+B", "Toggle GUI");
    addRow(ui, "H or ?", "Show help (this window)");
    addRow(ui, COMMAND + "+F", "Find image or channel group");
    addRow(ui, "Escape", "Reset find string");
    addRow(ui, COMMAND + "+Q", "Quit");

    // About tab
    Widget* about = new Widget(tabWidget);
    about->set_layout(new GroupLayout{});
    tabWidget->append_tab("About", about);

    auto addText = [](Widget* current, string text, string font = "sans", int fontSize = 18) {
        auto row = new Widget{current};
        row->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Middle, 0, 10});
        new Label{row, text, font, fontSize};
    };

    auto addLibrary = [](Widget* current, string name, string desc) {
        auto row = new Widget{current};
        row->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 3, 30});
        auto leftColumn = new Widget{row};
        leftColumn->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Maximum});
        leftColumn->set_fixed_width(135);

        new Label{leftColumn, name, "sans-bold", 18};
        new Label{row, desc, "sans", 18};
    };

    auto addSpacer = [](Widget* current, int space) {
        auto row = new Widget{current};
        row->set_height(space);
    };

    addSpacer(about, 5);

    addText(about, "tev — The EXR Viewer", "sans-bold", 46);
    addText(about, "version " TEV_VERSION, "sans", 26);

    addSpacer(about, 20);

    addText(about, "tev was developed by Thomas Müller <tom94.net> and is released under the BSD 3-Clause");
    addText(about, "License. It was built directly or indirectly upon the following amazing third-party libraries.");

    addSpacer(about, 20);

    addLibrary(about, "args", "Single-header argument parsing library");
#ifdef TEV_SUPPORT_AVIF
    addLibrary(about, "aom", "Alliance for Open Media Video Codec");
#endif
    addLibrary(about, "clip", "Cross-platform clipboard library");
    addLibrary(about, "{fmt}", "Fast & safe formatting library");
    addLibrary(about, "Glad", "Multi-language GL loader-generator");
    addLibrary(about, "GLFW", "OpenGL desktop development library");
#ifdef TEV_SUPPORT_HEIC
    addLibrary(about, "libde265", "Open h.265 video codec implementation.");
#endif
#ifdef TEV_USE_LIBHEIF
    addLibrary(about, "libheif", "HEIF and avif file format decoder and encoder");
    addLibrary(about, "Little-CMS", "FOSS CMM engine. Fast transforms between ICC profiles.");
#endif
    addLibrary(about, "NanoGUI", "Small GUI library");
    addLibrary(about, "NanoVG", "Small vector graphics library");
    addLibrary(about, "OpenEXR", "High dynamic-range (HDR) image file format");
    addLibrary(about, "qoi", "File format for fast, lossless image compression");
    addLibrary(about, "stb_image(_write)", "Single-header library for loading and writing images");
    addLibrary(about, "tinylogger", "Minimal pretty-logging library");
    addLibrary(about, "UTF8-CPP", "Lightweight UTF-8 string manipulation library");

    // Make the keybindings page as big as is needed to fit the about tab
    perform_layout(screen()->nvg_context());
    scrollPanel->set_fixed_height(about->height() + 12);

    tabWidget->set_selected_id(0);
    tabWidget->set_callback([tabWidget](int id) mutable { tabWidget->set_selected_id(id); });
}

bool HelpWindow::keyboard_event(int key, int scancode, int action, int modifiers) {
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
