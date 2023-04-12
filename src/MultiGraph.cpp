// This file was adapted from the nanogui::Graph class, which was developed
// by Wenzel Jakob <wenzel.jakob@epfl.ch> and based on the NanoVG demo application
// by Mikko Mononen. Modifications were developed by Thomas Müller <thomas94@gmx.net>.
// This file is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/MultiGraph.h>

#include <nanogui/theme.h>
#include <nanogui/opengl.h>

#include <array>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

MultiGraph::MultiGraph(Widget *parent, const std::string &caption)
: Widget(parent), mCaption(caption) {
    mBackgroundColor = Color(20, 128);
    mForegroundColor = Color(255, 192, 0, 128);
    mTextColor = Color(240, 192);
}

Vector2i MultiGraph::preferred_size(NVGcontext *) const {
    return Vector2i(180, 80);
}

void MultiGraph::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    NVGpaint bg = nvgBoxGradient(ctx,
        m_pos.x() + 1, m_pos.y() + 1 + 1.0f, m_size.x() - 2, m_size.y() - 2,
        3, 4, Color(120, 32), Color(32, 32));

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 1, m_pos.y() + 1 + 1.0f, m_size.x() - 2,
        m_size.y() - 2, 3);

    nvgFillPaint(ctx, bg);

    nvgFill(ctx);

    if (mValues.size() >= 2) {
        array<Color, 3> colors = {{
            Color{255, 0, 0, 200},
            Color{0, 255, 0, 200},
            Color{0, 0, 255, 200},
        }};

        nvgSave(ctx);
        // Additive blending
        nvgGlobalCompositeBlendFunc(ctx, NVGblendFactor::NVG_SRC_ALPHA, NVGblendFactor::NVG_ONE);

        size_t nBins = mValues.size() / mNChannels;

        for (size_t i = 0; i < (size_t)mNChannels; i++) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, m_pos.x(), m_pos.y() + m_size.y());

            for (size_t j = 0; j < (size_t)nBins; j++) {
                float value = mValues[j + i * nBins];
                float vx = m_pos.x() + 2 + j * (m_size.x() - 4) / (float)(nBins - 1);
                float vy = m_pos.y() + (1 - value) * m_size.y();
                nvgLineTo(ctx, vx, vy);
            }

            auto color = i < colors.size() ? colors[i] : mForegroundColor;
            nvgLineTo(ctx, m_pos.x() + m_size.x(), m_pos.y() + m_size.y());
            nvgFillColor(ctx, color);
            nvgFill(ctx);
        }

        nvgRestore(ctx);

        if (mZeroBin > 0) {
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x() + 1 + mZeroBin * (m_size.x() - 4) / (float)(nBins - 1), m_pos.y() + 15, 4, m_size.y() - 15);
            nvgFillColor(ctx, Color(0, 128));
            nvgFill(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x() + 2 + mZeroBin * (m_size.x() - 4) / (float)(nBins - 1), m_pos.y() + 15, 2, m_size.y() - 15);
            nvgFillColor(ctx, Color(200, 255));
            nvgFill(ctx);
        }

        nvgFontFace(ctx, "sans");

        nvgFontSize(ctx, 15.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        drawTextWithShadow(ctx, m_pos.x() + 3, m_pos.y() + 1, format("{:.3f}", mMinimum));

        nvgTextAlign(ctx, NVG_ALIGN_MIDDLE | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        string meanString = format("{:.3f}", mMean);
        float textWidth = nvgTextBounds(ctx, 0, 0, meanString.c_str(), nullptr, nullptr);
        drawTextWithShadow(ctx, m_pos.x() + m_size.x() / 2 - textWidth / 2, m_pos.y() + 1, meanString);

        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        drawTextWithShadow(ctx, m_pos.x() + m_size.x() - 3, m_pos.y() + 1, format("{:.3f}", mMaximum));

        if (!mCaption.empty()) {
            nvgFontSize(ctx, 14.0f);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, m_pos.x() + 3, m_pos.y() + 1, mCaption.c_str(), NULL);
        }

        if (!mHeader.empty()) {
            nvgFontSize(ctx, 18.0f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, m_pos.x() + m_size.x() - 3, m_pos.y() + 1, mHeader.c_str(), NULL);
        }

        if (!mFooter.empty()) {
            nvgFontSize(ctx, 15.0f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, m_pos.x() + m_size.x() - 3, m_pos.y() + m_size.y() - 1, mFooter.c_str(), NULL);
        }
    }

    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgRoundedRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y(), 2.5f);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillColor(ctx, Color(0.23f, 1.0f));
    nvgFill(ctx);

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1,
        m_size.y() - 1, 2.5f);
    nvgStrokeColor(ctx, Color(0, 48));
    nvgStroke(ctx);
}

TEV_NAMESPACE_END
