// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <nanogui/window.h>

#include <string>

namespace tev {

class HelpWindow : public nanogui::Window {
public:
    HelpWindow(nanogui::Widget* parent, bool supportsHdr, std::function<void()> closeCallback);

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    static std::string COMMAND;
    static std::string ALT;

private:
    std::function<void()> mCloseCallback;
};

}
