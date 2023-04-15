// This file was adapted from the nanogui::Graph class, which was developed
// by Wenzel Jakob <wenzel.jakob@epfl.ch> and based on the NanoVG demo application
// by Mikko Mononen. Modifications were developed by Thomas Müller <thomas94@gmx.net>.
// This file is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <nanogui/widget.h>

namespace tev {

class MultiGraph : public nanogui::Widget {
public:
    MultiGraph(nanogui::Widget *parent, const std::string &caption = "Untitled");

    const std::string &caption() const { return mCaption; }
    void setCaption(const std::string &caption) { mCaption = caption; }

    const std::string &header() const { return mHeader; }
    void setHeader(const std::string &header) { mHeader = header; }

    const std::string &footer() const { return mFooter; }
    void setFooter(const std::string &footer) { mFooter = footer; }

    const nanogui::Color &backgroundColor() const { return mBackgroundColor; }
    void setBackgroundColor(const nanogui::Color &backgroundColor) { mBackgroundColor = backgroundColor; }

    const nanogui::Color &foregroundColor() const { return mForegroundColor; }
    void setForegroundColor(const nanogui::Color &foregroundColor) { mForegroundColor = foregroundColor; }

    const nanogui::Color &textColor() const { return mTextColor; }
    void setTextColor(const nanogui::Color &textColor) { mTextColor = textColor; }

    const std::vector<float> &values() const { return mValues; }
    std::vector<float> &values() { return mValues; }
    void setValues(const std::vector<float> &values) { mValues = values; }

    void setNChannels(int nChannels) { mNChannels = nChannels; }

    virtual nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;
    virtual void draw(NVGcontext *ctx) override;

    void setMinimum(float minimum) {
        mMinimum = minimum;
    }

    void setMean(float mean) {
        mMean = mean;
    }

    void setMaximum(float maximum) {
        mMaximum = maximum;
    }

    void setZero(int zeroBin) {
        mZeroBin = zeroBin;
    }

protected:
    std::string mCaption, mHeader, mFooter;
    nanogui::Color mBackgroundColor, mForegroundColor, mTextColor;
    std::vector<float> mValues;
    int mNChannels = 1;
    float mMinimum = 0, mMean = 0, mMaximum = 0;
    int mZeroBin = 0;
};

}
