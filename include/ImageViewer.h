// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

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

    void drawContents() override;

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false);

    void selectImage(const std::shared_ptr<Image>& image);

    void selectLayer(std::string name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() {
        return mExposureSlider->value();
    }

    void setExposure(float value);

    float offset() {
        return mOffsetSlider->value();
    }

    void setOffset(float value);

    void normalizeExposureAndOffset();
    void resetImage();

    ETonemap tonemap() {
        return mImageCanvas->tonemap();
    }

    void setTonemap(ETonemap tonemap);

    EMetric metric() {
        return mImageCanvas->metric();
    }

    void setMetric(EMetric metric);

    void fitAllImages();
    bool setFilter(const std::string& filter);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() {
        return mSidebar->visible();
    }
    void setUiVisible(bool shouldBeVisible);

private:
    void updateLayout();
    void updateTitle();
    std::string layerName(size_t index);

    size_t layerId(const std::string& layer) const;
    size_t imageId(const std::shared_ptr<Image>& image) const;

    std::string nextLayer(const std::string& layer);
    std::string previousLayer(const std::string& layer);
    std::string nthVisibleLayer(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image);
    std::shared_ptr<Image> previousImage(const std::shared_ptr<Image>& image);
    std::shared_ptr<Image> nthVisibleImage(size_t n);

    nanogui::Widget* mVerticalScreenSplit;

    nanogui::Widget* mSidebar;
    nanogui::Widget* mFooter;

    nanogui::Label* mExposureLabel;
    nanogui::Slider* mExposureSlider;

    nanogui::Label* mOffsetLabel;
    nanogui::Slider* mOffsetSlider;

    nanogui::Widget* mTonemapButtonContainer;
    nanogui::Widget* mMetricButtonContainer;

    std::shared_ptr<Image> mCurrentImage;
    std::shared_ptr<Image> mCurrentReference;

    std::vector<std::shared_ptr<Image>> mImages;

    nanogui::TextBox* mFilter;

    nanogui::Widget* mImageButtonContainer;
    nanogui::Widget* mScrollContent;
    nanogui::VScrollPanel* mImageScrollContainer;

    ImageCanvas* mImageCanvas;

    nanogui::Widget* mLayerButtonContainer;
    std::string mCurrentLayer;
};

TEV_NAMESPACE_END
