/*
 * tev -- the EDR viewer
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

#pragma once

#include <tev/Common.h>

#include <nanogui/tabwidget.h>
#include <nanogui/vscrollpanel.h>
#include <nanogui/window.h>

#include <string>
#include <string_view>

#include <tev/Image.h>

namespace tev {

class ImageInfoWindow : public nanogui::Window {
public:
    ImageInfoWindow(nanogui::Widget* parent, const std::shared_ptr<Image>& image, std::function<void()> closeCallback);

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    static std::string COMMAND;
    static std::string ALT;

    std::string_view currentTabName() const {
        return mTabWidget && mTabWidget->tab_count() > 0 ? mTabWidget->tab_caption(mTabWidget->selected_id()) : std::string_view{};
    }

    bool selectTabWithName(const std::string_view name) {
        if (mTabWidget) {
            for (int i = 0; i < mTabWidget->tab_count(); ++i) {
                if (mTabWidget->tab_caption(mTabWidget->tab_id(i)) == name) {
                    mTabWidget->set_selected_index(i);
                    return true;
                }
            }
        }

        return false;
    }

    float currentScroll() const { return mScrollPanel ? mScrollPanel->scroll() : 0.0f; }
    void setScroll(const float scroll) {
        if (mScrollPanel) {
            mScrollPanel->set_scroll(scroll);
        }
    }

private:
    std::function<void()> mCloseCallback;
    nanogui::TabWidget* mTabWidget = nullptr;
    nanogui::VScrollPanel* mScrollPanel = nullptr;
};

} // namespace tev
