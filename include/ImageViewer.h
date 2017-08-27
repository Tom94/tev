// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include "../include/Image.h"
#include "../include/ImageButton.h"
#include "../include/ImageCanvas.h"

#include <nanogui/glutil.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>

#include <vector>
#include <memory>

TEV_NAMESPACE_BEGIN

class ImageViewer : public nanogui::Screen {
public:
    ImageViewer();

    bool dropEvent(const std::vector<std::string>& filenames) override;

    bool keyboardEvent(int key, int scancode, int action, int modifiers) override;

    void draw(NVGcontext *ctx) override;

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false);

    void tryLoadImage(const std::string& filename, bool shallSelect = false);

    void selectImage(size_t index);

    void unselectReference();
    void selectReference(size_t index);

    float exposure() {
        return mExposureSlider->value();
    }

    void setExposure(float value);

    ETonemap tonemap() {
        return mImageCanvas->tonemap();
    }

    void setTonemap(ETonemap tonemap);

    EMetric metric() {
        return mImageCanvas->metric();
    }

    void setMetric(EMetric metric);

    void fitAllImages();
    void maximize();

private:

    struct ImageInfo {
        std::shared_ptr<Image> image;
        nanogui::ref<ImageButton> button;
    };

    int mMenuWidth = 200;

    nanogui::Label* mExposureLabel;
    nanogui::Slider* mExposureSlider;

    size_t mCurrentImage = 0;
    size_t mCurrentReference = 0;

    std::vector<ImageInfo> mImageInfos;
    nanogui::Widget* mImageButtonContainer;
    nanogui::VScrollPanel* mImageScrollContainer;

    ImageCanvas* mImageCanvas;

    nanogui::Widget* mMetricButtonContainer;
    nanogui::Widget* mTonemapButtonContainer;
};

TEV_NAMESPACE_END
