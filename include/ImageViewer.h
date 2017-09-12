// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/HelpWindow.h"
#include "../include/Image.h"
#include "../include/ImageButton.h"
#include "../include/ImageCanvas.h"
#include "../include/SharedQueue.h"

#include <nanogui/glutil.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>

#include <vector>
#include <memory>

TEV_NAMESPACE_BEGIN

struct ImageAddition {
    bool shallSelect;
    std::shared_ptr<Image> image;
};

class ImageViewer : public nanogui::Screen {
public:
    ImageViewer();
    ImageViewer(std::shared_ptr<SharedQueue<ImageAddition>> imagesToAdd);

    bool dropEvent(const std::vector<std::string>& filenames) override;

    bool keyboardEvent(int key, int scancode, int action, int modifiers) override;

    void drawContents() override;

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) {
        insertImage(image, mImages.size(), shallSelect);
    }

    void removeImage(std::shared_ptr<Image> image);

    void reloadImage(std::shared_ptr<Image> image);

    void reloadAllImages();

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

    void resizeToFitImage(const std::shared_ptr<Image>& image);
    void resizeToFitAllImages();
    bool setFilter(const std::string& filter);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() {
        return mSidebar->visible();
    }
    void setUiVisible(bool shouldBeVisible);

    void toggleHelpWindow();

    void openImageDialog();

    void requestLayoutUpdate() {
        mRequiresLayoutUpdate = true;
    }

private:
    void updateFilter();
    void updateLayout();
    void updateTitle();
    std::string layerName(size_t index);

    int layerId(const std::string& layer) const;
    int imageId(const std::shared_ptr<Image>& image) const;

    std::string nextLayer(const std::string& layer, EDirection direction);
    std::string nthVisibleLayer(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image, EDirection direction);
    std::shared_ptr<Image> nthVisibleImage(size_t n);

    bool mRequiresFilterUpdate = true;
    bool mRequiresLayoutUpdate = true;

    nanogui::Widget* mVerticalScreenSplit;

    nanogui::Widget* mSidebar;
    nanogui::Widget* mFooter;

    nanogui::Label* mExposureLabel;
    nanogui::Slider* mExposureSlider;

    nanogui::Label* mOffsetLabel;
    nanogui::Slider* mOffsetSlider;

    nanogui::Widget* mTonemapButtonContainer;
    nanogui::Widget* mMetricButtonContainer;

    std::shared_ptr<SharedQueue<ImageAddition>> mImagesToAdd;
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

    HelpWindow* mHelpWindow = nullptr;
};

TEV_NAMESPACE_END
