#pragma once

#include <nanogui/widget.h>

namespace tev {

class SplitscreenSlider : public nanogui::Widget {
public:
    SplitscreenSlider();

    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel, int button, int modifiers) override;
    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down, int modifiers) override;
    void draw(NVGcontext* ctx) override;

    const std::function<void(int)> &callback() const { return mCallback; }
    void set_callback(const std::function<void(int)> &callback) { mCallback = callback; }

    void set_range(int begin, int end) { mRange = { begin, end }; }

protected:
    int mValue;
    int mHandleHeight;
    std::function<void(int)> mCallback;
    std::pair<int, int> mRange;
    bool mDrag;
};

} // namespace tev