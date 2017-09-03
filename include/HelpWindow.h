// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/Common.h"

#include <nanogui/window.h>

#include <string>

TEV_NAMESPACE_BEGIN

class HelpWindow : public nanogui::Window {
public:
    HelpWindow(nanogui::Widget* parent, std::function<void()> closeCallback);

    static std::string COMMAND;
    static std::string ALT;
};

TEV_NAMESPACE_END
