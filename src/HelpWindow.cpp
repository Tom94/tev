// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/HelpWindow.h>

#include <nanogui/button.h>
#include <nanogui/entypo.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/tabwidget.h>
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
    : Window{parent, "Help"}, mCloseCallback{closeCallback} {

    auto closeButton = new Button{buttonPanel(), "", ENTYPO_ICON_CROSS};
    closeButton->setCallback(mCloseCallback);

    setLayout(new GroupLayout{});
    setFixedWidth(600);

    TabWidget* tabWidget = new TabWidget{this};

    // Keybindings tab
    {
        Widget* shortcuts = tabWidget->createTab("Keybindings");
        shortcuts->setLayout(new GroupLayout{});

        auto addRow = [](Widget* current, string keys, string desc) {
            auto row = new Widget{current};
            row->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 0, 10});
            auto descWidget = new Label{row, desc, "sans"};
            descWidget->setFixedWidth(250);
            new Label{row, keys, "sans-bold"};
        };

        new Label{shortcuts, "Image Loading", "sans-bold", 18};
        auto imageLoading = new Widget{shortcuts};
        imageLoading->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

        addRow(imageLoading, COMMAND + "+O",                             "Open Image");
        addRow(imageLoading, COMMAND + "+S",                             "Save View as Image");
        addRow(imageLoading, COMMAND + "+R or F5",                       "Reload Image");
        addRow(imageLoading, COMMAND + "+Shift+R or " + COMMAND + "+F5", "Reload All Images");
        addRow(imageLoading, COMMAND + "+W",                             "Close Image");
        addRow(imageLoading, COMMAND + "+Shift+W",                       "Close All Images");
        addRow(imageLoading, COMMAND + "+C",                             "Copy Image or Path to Clipboard");
        addRow(imageLoading, COMMAND + "+V",                             "Paste Image from Clipboard");

        new Label{shortcuts, "Image Options", "sans-bold", 18};
        auto imageSelection = new Widget{shortcuts};
        imageSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

        addRow(imageSelection, "Left Click",          "Select Hovered Image");
        addRow(imageSelection, "1…9",                 "Select N-th Image");
        addRow(imageSelection, "Down or S / Up or W", "Select Next / Previous Image");

        addRow(imageSelection, "Click & Drag (+Shift/" + COMMAND + ")", "Translate Image");
        addRow(imageSelection, "Plus / Minus / Scroll (+Shift/" + COMMAND + ")", "Zoom In / Out of Image");

        addRow(imageSelection, "F", "Fit Image to Screen");
        addRow(imageSelection, "N", "Normalize Image to [0, 1]");
        addRow(imageSelection, "R", "Reset Image Parameters");

        addRow(imageSelection, "Shift+Right or Shift+D / Shift+Left or Shift+A", "Select Next / Previous Tonemap");

        addRow(imageSelection, "E / Shift+E", "Increase / Decrease Exposure by 0.5");
        addRow(imageSelection, "O / Shift+O", "Increase / Decrease Offset by 0.1");

        addRow(imageSelection, ALT + " (hold)", "Display raw bytes on pixels when zoomed-in");

        new Label{shortcuts, "Reference Options", "sans-bold", 18};
        auto referenceSelection = new Widget{shortcuts};
        referenceSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

        addRow(referenceSelection, "Shift (hold)",                                "View currently selected Reference");
        addRow(referenceSelection, "Shift+Left Click or Right Click",             "Select Hovered Image as Reference");
        addRow(referenceSelection, "Shift+1…9",                                   "Select N-th Image as Reference");
        addRow(referenceSelection, "Shift+Down or Shift+S / Shift+Up or Shift+W", "Select Next / Previous Image as Reference");

        addRow(referenceSelection, "Ctrl (hold)",                                "View selected Image if Reference is selected");
        addRow(referenceSelection, "Ctrl+Right or Ctrl+D / Ctrl+Left or Ctrl+A", "Select Next / Previous Error Metric");

        new Label{shortcuts, "Channel Group Options", "sans-bold", 18};
        auto groupSelection = new Widget{shortcuts};
        groupSelection->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

        addRow(groupSelection, "Left Click",             "Select Hovered Channel Group");
        addRow(groupSelection, "Ctrl+1…9",               "Select N-th Channel Group");
        addRow(groupSelection, "Right or D / Left or A", "Select Next / Previous Channel Group");

        new Label{shortcuts, "Interface", "sans-bold", 18};
        auto ui = new Widget{shortcuts};
        ui->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

        addRow(ui, ALT + "+Enter", "Maximize");
        addRow(ui, COMMAND + "+B", "Toggle GUI");
        addRow(ui, "H",            "Show Help (this Window)");
        addRow(ui, COMMAND + "+P", "Find Image or Channel Group");
        addRow(ui, "Q or Esc",     "Quit");
    }

    // About tab
    {
        Widget* about = tabWidget->createTab("About");
        about->setLayout(new GroupLayout{});

        auto addText = [](Widget* current, string text, string font = "sans", int fontSize = 18) {
            auto row = new Widget{current};
            row->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Middle, 0, 10});
            new Label{row, text, font, fontSize };
        };

        auto addLibrary = [](Widget* current, string name, string license, string desc) {
            auto row = new Widget{current};
            row->setLayout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 3, 30});
            auto leftColumn = new Widget{row};
            leftColumn->setLayout(new BoxLayout{Orientation::Vertical, Alignment::Maximum});
            leftColumn->setFixedWidth(135);

            new Label{leftColumn, name, "sans-bold", 18};
            new Label{row, desc, "sans", 18};
        };

        auto addSpacer = [](Widget* current, int space) {
            auto row = new Widget{current};
            row->setHeight(space);
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
        addLibrary(about, "Eigen",             "", "C++ Template Library for Linear Algebra");
        addLibrary(about, "filesystem",        "", "Lightweight Path Manipulation Library");
        addLibrary(about, "Glad",              "", "Multi-Language GL Loader-Generator");
        addLibrary(about, "GLEW",              "", "The OpenGL Extension Wrangler Library");
        addLibrary(about, "GLFW",              "", "OpenGL Desktop Development Library");
        addLibrary(about, "NanoGUI",           "", "Small Widget Library for OpenGL");
        addLibrary(about, "NanoVG",            "", "Small Vector Graphics Library");
        addLibrary(about, "OpenEXR",           "", "High Dynamic-Range (HDR) Image File Format");
        addLibrary(about, "stb_image(_write)", "", "Single-Header Library for Loading and Writing Images");
        addLibrary(about, "tinyformat",        "", "Minimal Type-Safe printf() Replacement");
        addLibrary(about, "tinylogger",        "", "Minimal Pretty-Logging Library");
        addLibrary(about, "UTF8-CPP",          "", "Lightweight UTF-8 String Manipulation Library");
    }

    tabWidget->setActiveTab(0);
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
