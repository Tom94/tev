// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <nanogui/window.h>

#include <string>

#include <tev/Image.h>

namespace tev {

class ImageInfoWindow : public nanogui::Window {
public:
    ImageInfoWindow(nanogui::Widget* parent, const std::shared_ptr<Image>& image, bool supportsHdr, std::function<void()> closeCallback);

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    static std::string COMMAND;
    static std::string ALT;

private:
    std::function<void()> mCloseCallback;
};

}
