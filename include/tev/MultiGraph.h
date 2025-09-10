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

// This file was adapted from the nanogui::Graph class, which was developed
// by Wenzel Jakob <wenzel.jakob@epfl.ch> and based on the NanoVG demo application
// by Mikko Mononen. Modifications were developed by Thomas Müller <contact@tom94.net>.

#pragma once

#include <tev/Common.h>

#include <nanogui/widget.h>

#include <span>

namespace tev {

class MultiGraph : public nanogui::Widget {
public:
    MultiGraph(nanogui::Widget* parent, std::string_view caption = "Untitled");

    std::string_view caption() const { return mCaption; }
    void setCaption(std::string_view caption) { mCaption = caption; }

    std::string_view header() const { return mHeader; }
    void setHeader(std::string_view header) { mHeader = header; }

    std::string_view footer() const { return mFooter; }
    void setFooter(std::string_view footer) { mFooter = footer; }

    const nanogui::Color& backgroundColor() const { return mBackgroundColor; }
    void setBackgroundColor(const nanogui::Color& backgroundColor) { mBackgroundColor = backgroundColor; }

    const nanogui::Color& foregroundColor() const { return mForegroundColor; }
    void setForegroundColor(const nanogui::Color& foregroundColor) { mForegroundColor = foregroundColor; }

    const nanogui::Color& textColor() const { return mTextColor; }
    void setTextColor(const nanogui::Color& textColor) { mTextColor = textColor; }

    std::span<const float> values() const { return mValues; }
    void setValues(std::span<const float> values) { mValues = {values.begin(), values.end()}; }

    std::span<nanogui::Color> colors() { return mColors; }
    void setColors(std::span<const nanogui::Color> colors) { mColors = {colors.begin(), colors.end()}; }

    void setNChannels(int nChannels) { mNChannels = nChannels; }

    virtual nanogui::Vector2i preferred_size_impl(NVGcontext* ctx) const override;
    virtual void draw(NVGcontext* ctx) override;

    void setMinimum(float minimum) { mMinimum = minimum; }

    void setMean(float mean) { mMean = mean; }

    void setMaximum(float maximum) { mMaximum = maximum; }

    void setZero(int zeroBin) { mZeroBin = zeroBin; }

protected:
    std::string mCaption, mHeader, mFooter;
    nanogui::Color mBackgroundColor, mForegroundColor, mTextColor;
    std::vector<float> mValues;
    std::vector<nanogui::Color> mColors;
    int mNChannels = 1;
    float mMinimum = 0, mMean = 0, mMaximum = 0;
    int mZeroBin = 0;
};

} // namespace tev
