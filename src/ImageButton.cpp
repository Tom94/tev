// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/ImageButton.h"

using namespace nanogui;

ImageButton::ImageButton(NVGcontext* nvgContext, nanogui::Widget* parent, const std::string& caption, int icon)
    : Button(parent, caption, icon) {

    ref<Theme> theme = new Theme{nvgContext};
    theme->mButtonCornerRadius = 5;
    theme->mBorderDark = Color(0, 0, 0, 0);
    theme->mBorderMedium = Color(0, 0, 0, 0);
    theme->mBorderLight = Color(0, 0, 0, 0);
    theme->mButtonGradientBotUnfocused = Color(0, 0, 0, 0);
    theme->mButtonGradientTopUnfocused = Color(0, 0, 0, 0);
    setTheme(theme);

    setCaption("..." + caption.substr(caption.length() - 30));
}

