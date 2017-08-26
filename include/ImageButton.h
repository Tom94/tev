// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include <nanogui/button.h>
#include <nanogui/theme.h>

class ImageButton : public nanogui::Button {
public:
    ImageButton(NVGcontext* nvgContext, nanogui::Widget* parent, const std::string& caption = "Untitled", int icon = 0);

};
