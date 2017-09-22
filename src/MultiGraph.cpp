// This file was adapted from the nanogui::Graph class, which was developed
// by Wenzel Jakob <wenzel.jakob@epfl.ch> and based on the NanoVG demo application
// by Mikko Mononen. Modifications were developed by Thomas Müller <thomas94@gmx.net>.
// This file is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/MultiGraph.h>

#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <nanogui/serializer/core.h>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

MultiGraph::MultiGraph(Widget *parent, const std::string &caption)
: Widget(parent), mCaption(caption) {
    mBackgroundColor = Color(20, 128);
    mForegroundColor = Color(255, 192, 0, 128);
    mTextColor = Color(240, 192);
}

Vector2i MultiGraph::preferredSize(NVGcontext *) const {
    return Vector2i(180, 80);
}

void MultiGraph::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    NVGpaint bg = nvgBoxGradient(ctx,
        mPos.x() + 1, mPos.y() + 1 + 1.0f, mSize.x() - 2, mSize.y() - 2,
        3, 4, Color(120, 32), Color(32, 32));

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, mPos.x() + 1, mPos.y() + 1 + 1.0f, mSize.x() - 2,
        mSize.y() - 2, 3);

    nvgFillPaint(ctx, bg);

    nvgFill(ctx);

    if (mValues.cols() >= 1 && mValues.rows() >= 2) {
        array<Color, 3> colors = {{
            Color{255, 0, 0, 200},
            Color{0, 255, 0, 200},
            Color{0, 0, 255, 200},
        }};

        nvgSave(ctx);
        // Additive blending
        nvgGlobalCompositeBlendFunc(ctx, NVGblendFactor::NVG_SRC_ALPHA, NVGblendFactor::NVG_ONE);

        for (size_t i = 0; i < (size_t)mValues.cols(); i++) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, mPos.x(), mPos.y() + mSize.y());
            
            for (size_t j = 0; j < (size_t)mValues.rows(); j++) {
                float value = mValues(j, i);
                float vx = mPos.x() + 2 + j * (mSize.x() - 4) / (float)(mValues.rows() - 1);
                float vy = mPos.y() + (1 - value) * mSize.y();
                nvgLineTo(ctx, vx, vy);
            }

            auto color = i < colors.size() ? colors[i] : mForegroundColor;
            nvgLineTo(ctx, mPos.x() + mSize.x(), mPos.y() + mSize.y());
            nvgFillColor(ctx, colors[i]);
            nvgFill(ctx);
        }

        nvgRestore(ctx);

        if (mZeroBin > 0) {
            nvgBeginPath(ctx);
            nvgRect(ctx, mPos.x() + 2 + mZeroBin * (mSize.x() - 4) / (float)(mValues.rows() - 1), mPos.y() + 15, 1, mSize.y() - 15);
            nvgFillColor(ctx, Color(200, 255));
            nvgFill(ctx);
        }

        nvgFontFace(ctx, "sans");

        nvgFontSize(ctx, 14.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        nvgText(ctx, mPos.x() + 3, mPos.y() + 1, tfm::format("%.3f", mMinimum).c_str(), NULL);

        nvgTextAlign(ctx, NVG_ALIGN_MIDDLE | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        string meanString = tfm::format("%.3f", mMean);
        float textWidth = nvgTextBounds(ctx, 0, 0, meanString.c_str(), nullptr, nullptr);
        nvgText(ctx, mPos.x() + mSize.x() / 2 - textWidth / 2, mPos.y() + 1, meanString.c_str(), NULL);

        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, mTextColor);
        nvgText(ctx, mPos.x() + mSize.x() - 3, mPos.y() + 1, tfm::format("%.3f", mMaximum).c_str(), NULL);

        if (!mCaption.empty()) {
            nvgFontSize(ctx, 14.0f);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, mPos.x() + 3, mPos.y() + 1, mCaption.c_str(), NULL);
        }

        if (!mHeader.empty()) {
            nvgFontSize(ctx, 18.0f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, mPos.x() + mSize.x() - 3, mPos.y() + 1, mHeader.c_str(), NULL);
        }

        if (!mFooter.empty()) {
            nvgFontSize(ctx, 15.0f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
            nvgFillColor(ctx, mTextColor);
            nvgText(ctx, mPos.x() + mSize.x() - 3, mPos.y() + mSize.y() - 1, mFooter.c_str(), NULL);
        }
    }

    nvgBeginPath(ctx);
    nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
    nvgRoundedRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y(), 2.5f);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillColor(ctx, Color(0.23f, 1.0f));
    nvgFill(ctx);

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, mPos.x() + 0.5f, mPos.y() + 0.5f, mSize.x() - 1,
        mSize.y() - 1, 2.5f);
    nvgStrokeColor(ctx, Color(0, 48));
    nvgStroke(ctx);
}

TEV_NAMESPACE_END
