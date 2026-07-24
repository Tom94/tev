#include <nanogui/opengl.h>
#include <tev/SplitscreenSlider.h>
#include <tinylogger/tinylogger.h>

namespace tev {

SplitscreenSlider::SplitscreenSlider() : Widget(nullptr), mValue(0), mHandleHeight(0), mCallback(nullptr), mRange(0, 0), mDrag(false) {}

bool SplitscreenSlider::mouse_drag_event(const nanogui::Vector2i& p, const nanogui::Vector2i& rel, int button, int modifiers) {
    if (!m_enabled) {
        return false;
    }

    if (mDrag && (button & (1 << GLFW_MOUSE_BUTTON_RIGHT)) != 0) {
        mHandleHeight = p.y();
        int value = p.x() - mRange.first, old_value = mValue;
        mValue = std::min(std::max(value, 0), mRange.second);
        if (mCallback && mValue != old_value) {
            mCallback(mValue);
        }
        return true;
    }
    return false;
}
bool SplitscreenSlider::mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) {
    if (!m_enabled) {
        return false;
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        mDrag = down;
        mHandleHeight = p.y();
        int value = p.x() - mRange.first, old_value = mValue;
        mValue = std::min(std::max(value, 0), mRange.second);
        if (mCallback && mValue != old_value) {
            mCallback(mValue);
        }
        return true;
    }

    return false;
}
void SplitscreenSlider::draw(NVGcontext* ctx) {
    nanogui::Vector2f centre = nanogui::Vector2f(m_pos) + nanogui::Vector2f(m_size) * 0.5f;

    constexpr float innerrad = 6.0f;
    float cx = centre.x();
    float cy = static_cast<float>(mHandleHeight) - innerrad * 0.5f - 2.0f;

    NVGcolor lightGrey = nvgRGBA(203, 208, 204, 255);

    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), cy - innerrad);
    nvgRect(ctx, m_pos.x(), m_pos.y() + cy + innerrad, m_size.x(), m_size.y() - cy - innerrad);
    nvgFillColor(ctx, lightGrey);
    nvgFill(ctx);

    float altitude = 8.0f;
    float hh = altitude * 5.0f;
    float top = cy - hh;
    float bot = cy + hh;
    float peakr = cx + altitude;
    float peakl = cx - altitude;
    float s1 = 1.0f, s2 = 0.4f;

    nvgBeginPath(ctx);
    nvgMoveTo(ctx, cx, top);

    // top half
    nvgBezierTo(ctx, cx, top + hh * s1, peakr, cy - hh * s2, peakr, cy);

    // bot half
    nvgBezierTo(ctx, peakr, cy + hh * s2, cx, bot - hh * s1, cx, bot);

    // bot half
    nvgBezierTo(ctx, cx, bot - hh * s1, peakl, cy + hh * s2, peakl, cy);

    // top half
    nvgBezierTo(ctx, peakl, cy - hh * s2, cx, top + hh * s1, cx, top);

    nvgClosePath(ctx);

    nvgCircle(ctx, cx, cy, innerrad);
    nvgPathWinding(ctx, NVG_HOLE);

    nvgFillColor(ctx, lightGrey);
    nvgFill(ctx);
}

} // namespace tev
