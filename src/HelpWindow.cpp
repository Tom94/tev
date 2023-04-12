// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

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

HelpWindow::HelpWindow(Widget* parent, bool supportsHdr, function<void()> closeCallback)
    : Window{parent, "Help"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{button_panel(), "", FA_TIMES};
    closeButton->set_callback(mCloseCallback);

    set_layout(new GroupLayout{});
    set_fixed_width(600);

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

    addRow(imageLoading, COMMAND + "+O",                             "Open image");
    addRow(imageLoading, COMMAND + "+S",                             "Save view as image");
    addRow(imageLoading, COMMAND + "+R or F5",                       "Reload image");
    addRow(imageLoading, COMMAND + "+Shift+R or " + COMMAND + "+F5", "Reload all images");
    addRow(imageLoading, COMMAND + "+W",                             "Close image");
    addRow(imageLoading, COMMAND + "+Shift+W",                       "Close all images");
    addRow(imageLoading, COMMAND + "+C",                             "Copy image to clipboard");
    addRow(imageLoading, COMMAND + "+Shift+C",                       "Copy image's path to clipboard");
    addRow(imageLoading, COMMAND + "+V",                             "Paste image from clipboard");

    new Label{shortcuts, "Image options", "sans-bold", 18};
    auto imageSelection = new Widget{shortcuts};
    imageSelection->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(imageSelection, "Left Click",          "Select hovered image");
    addRow(imageSelection, "1…9",                 "Select N-th image");
    addRow(imageSelection, "Down or S / Up or W", "Select next / previous image");
    addRow(imageSelection, "Home / End",          "Select first / last image");
    addRow(imageSelection, "Space",               "Toggle playback of images as video");

    addRow(imageSelection, "Click & Drag (+Shift/" + COMMAND + ")", "Translate image");
    addRow(imageSelection, "+ / - / Scroll (+Shift/" + COMMAND + ")", "Zoom in / out of image");

    addRow(imageSelection, COMMAND + "+0", "Zoom to actual size");
    addRow(imageSelection, COMMAND + "+9 / F", "Zoom to fit");
    addRow(imageSelection, "N", "Normalize image to [0, 1]");
    addRow(imageSelection, "R", "Reset image parameters");
    if (supportsHdr) {
        addRow(imageSelection, "L", "Display the image as if on an LDR screen");
    }

    addRow(imageSelection, "Shift+Right or Shift+D / Shift+Left or Shift+A", "Select next / previous tonemap");

    addRow(imageSelection, "E / Shift+E", "Increase / decrease exposure by 0.5");
    addRow(imageSelection, "O / Shift+O", "Increase / decrease offset by 0.1");

    addRow(imageSelection, "B (hold)",          "Draw a border around the image");
    addRow(imageSelection, "Shift+Ctrl (hold)", "Display raw bytes on pixels when zoomed-in");

    new Label{shortcuts, "Reference options", "sans-bold", 18};
    auto referenceSelection = new Widget{shortcuts};
    referenceSelection->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(referenceSelection, ALT + " (hold)",                               "View currently selected reference");
    addRow(referenceSelection, "Shift+Left Click or Right Click",             "Select hovered image as reference");
    addRow(referenceSelection, "Shift+1…9",                                   "Select N-th image as reference");
    addRow(referenceSelection, "Shift+Down or Shift+S / Shift+Up or Shift+W", "Select next / previous image as reference");

    addRow(referenceSelection, "Ctrl (hold)",                                "View selected image if reference is selected");
    addRow(referenceSelection, "Ctrl+Right or Ctrl+D / Ctrl+Left or Ctrl+A", "Select next / previous error metric");

    new Label{shortcuts, "Channel group options", "sans-bold", 18};
    auto groupSelection = new Widget{shortcuts};
    groupSelection->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(groupSelection, "Left Click",             "Select hovered channel group");
    addRow(groupSelection, "Ctrl+1…9",               "Select N-th channel group");
    addRow(groupSelection, "Right or D / Left or A", "Select next / previous channel group");

    new Label{shortcuts, "Interface", "sans-bold", 18};
    auto ui = new Widget{shortcuts};
    ui->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    addRow(ui, ALT + "+Enter", "Maximize");
    addRow(ui, COMMAND + "+B", "Toggle GUI");
    addRow(ui, "H",            "Show help (this window)");
    addRow(ui, COMMAND + "+P", "Find image or channel group");
    addRow(ui, "Escape",       "Reset find string");
    addRow(ui, COMMAND + "+Q", "Quit");

    // About tab
    Widget* about = new Widget(tabWidget);
    about->set_layout(new GroupLayout{});
    tabWidget->append_tab("About", about);

    auto addText = [](Widget* current, string text, string font = "sans", int fontSize = 18) {
        auto row = new Widget{current};
        row->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Middle, 0, 10});
        new Label{row, text, font, fontSize };
    };

    auto addLibrary = [](Widget* current, string name, string license, string desc) {
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

    addSpacer(about, 15);

    addText(about, "tev — The EXR Viewer", "sans-bold", 46);
    addText(about, "version " TEV_VERSION, "sans", 26);

    addSpacer(about, 50);

    addText(about, "tev was developed by Thomas Müller and is released under the BSD 3-Clause License.");
    addText(about, "It was built directly or indirectly upon the following amazing third-party libraries.");

    addSpacer(about, 30);

    addLibrary(about, "args",              "", "Single-Header Argument Parsing Library");
    addLibrary(about, "clip",              "", "Cross-Platform Clipboard Library");
    addLibrary(about, "Glad",              "", "Multi-Language GL Loader-Generator");
    addLibrary(about, "GLFW",              "", "OpenGL Desktop Development Library");
    addLibrary(about, "NanoGUI",           "", "Small GUI Library");
    addLibrary(about, "NanoVG",            "", "Small Vector Graphics Library");
    addLibrary(about, "OpenEXR",           "", "High Dynamic-Range (HDR) Image File Format");
    addLibrary(about, "qoi",               "", "File Format for Fast, Lossless Image Compression");
    addLibrary(about, "stb_image(_write)", "", "Single-Header Library for Loading and Writing Images");
    addLibrary(about, "tinylogger",        "", "Minimal Pretty-Logging Library");
    addLibrary(about, "UTF8-CPP",          "", "Lightweight UTF-8 String Manipulation Library");

    // Make the keybindings page as big as is needed to fit the about tab
    perform_layout(screen()->nvg_context());
    scrollPanel->set_fixed_height(about->height() + 12);

    tabWidget->set_selected_id(0);
    tabWidget->set_callback([tabWidget] (int id) mutable {
        tabWidget->set_selected_id(id);
    });
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

TEV_NAMESPACE_END
