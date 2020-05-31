// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/HelpWindow.h>
#include <tev/Image.h>
#include <tev/ImageButton.h>
#include <tev/ImageCanvas.h>
#include <tev/Lazy.h>
#include <tev/MultiGraph.h>
#include <tev/SharedQueue.h>

#include <nanogui/glutil.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/textbox.h>

#include <memory>
#include <vector>

TEV_NAMESPACE_BEGIN

class ImageViewer : public nanogui::Screen {
public:
    ImageViewer(const std::shared_ptr<BackgroundImagesLoader>& imagesLoader, bool processPendingDrops);
    virtual ~ImageViewer();

    bool mouseButtonEvent(const Eigen::Vector2i &p, int button, bool down, int modifiers) override;
    bool mouseMotionEvent(const Eigen::Vector2i& p, const Eigen::Vector2i& rel, int button, int modifiers) override;

    bool dropEvent(const std::vector<std::string>& filenames) override;

    bool keyboardEvent(int key, int scancode, int action, int modifiers) override;

    void focusWindow();

    void drawContents() override;

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);
    void moveImageInList(size_t oldIndex, size_t newIndex);

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) {
        insertImage(image, mImages.size(), shallSelect);
    }

    void removeImage(std::shared_ptr<Image> image);
    void removeImage(const std::string& imageName) {
        removeImage(imageByName(imageName));
    }
    void removeAllImages();

    void reloadImage(std::shared_ptr<Image> image, bool shallSelect = false);
    void reloadImage(const std::string& imageName, bool shallSelect = false) {
        reloadImage(imageByName(imageName), shallSelect);
    }
    void reloadAllImages();

    void updateImage(
        const std::string& imageName,
        bool shallSelect,
        const std::string& channel,
        int x, int y,
        int width, int height,
        const std::vector<float>& imageData
    );

    void selectImage(const std::shared_ptr<Image>& image, bool stopPlayback = true);

    void selectGroup(std::string name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() {
        return mExposureSlider->value();
    }

    void setExposure(float value);

    float offset() {
        return mOffsetSlider->value();
    }

    void setOffset(float value);

    float gamma() {
        return mGammaSlider->value();
    }

    void setGamma(float value);

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

    bool useRegex();
    void setUseRegex(bool value);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() {
        return mSidebar->visible();
    }
    void setUiVisible(bool shouldBeVisible);

    void toggleHelpWindow();

    void openImageDialog();
    void saveImageDialog();

    void requestLayoutUpdate() {
        mRequiresLayoutUpdate = true;
    }

    template <typename T>
    void scheduleToUiThread(const T& fun) {
        mTaskQueue.push(fun);
    }

private:
    void updateFilter();
    void updateLayout();
    void updateTitle();
    std::string groupName(size_t index);

    int groupId(const std::string& groupName) const;
    int imageId(const std::shared_ptr<Image>& image) const;
    int imageId(const std::string& imageName) const;

    std::string nextGroup(const std::string& groupName, EDirection direction);
    std::string nthVisibleGroup(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image, EDirection direction);
    std::shared_ptr<Image> nthVisibleImage(size_t n);
    std::shared_ptr<Image> imageByName(const std::string& imageName);

    bool canDragSidebarFrom(const Eigen::Vector2i& p) {
        return mSidebar->visible() && p.x() - mSidebar->fixedWidth() < 10 && p.x() - mSidebar->fixedWidth() > -5;
    }

    int visibleSidebarWidth() {
        return mSidebar->visible() ? mSidebar->fixedWidth() : 0;
    }

    int visibleFooterHeight() {
        return mFooter->visible() ? mFooter->fixedHeight() : 0;
    }

    SharedQueue<std::function<void(void)>> mTaskQueue;

    bool mRequiresFilterUpdate = true;
    bool mRequiresLayoutUpdate = true;

    nanogui::Widget* mVerticalScreenSplit;

    nanogui::Widget* mSidebar;
    nanogui::Button* mHelpButton;
    nanogui::Widget* mSidebarLayout;

    nanogui::Widget* mFooter;

    nanogui::Label* mExposureLabel;
    nanogui::Slider* mExposureSlider;

    nanogui::Label* mOffsetLabel;
    nanogui::Slider* mOffsetSlider;

    nanogui::Label* mGammaLabel;
    nanogui::Slider* mGammaSlider;

    nanogui::Widget* mTonemapButtonContainer;
    nanogui::Widget* mMetricButtonContainer;

    std::shared_ptr<BackgroundImagesLoader> mImagesLoader;

    std::shared_ptr<Image> mCurrentImage;
    std::shared_ptr<Image> mCurrentReference;

    std::vector<std::shared_ptr<Image>> mImages;

    MultiGraph* mHistogram;

    nanogui::TextBox* mFilter;
    nanogui::Button* mRegexButton;

    // Buttons which require a current image to be meaningful.
    std::vector<nanogui::Button*> mCurrentImageButtons;

    // Buttons which require at least one image to be meaningful
    std::vector<nanogui::Button*> mAnyImageButtons;

    nanogui::Button* mPlayButton;
    nanogui::IntBox<int>* mFpsTextBox;
    std::thread mPlaybackThread;
    bool mShallRunPlaybackThread = true;

    nanogui::Widget* mImageButtonContainer;
    nanogui::Widget* mScrollContent;
    nanogui::VScrollPanel* mImageScrollContainer;

    ImageCanvas* mImageCanvas;

    nanogui::Widget* mGroupButtonContainer;
    std::string mCurrentGroup;

    HelpWindow* mHelpWindow = nullptr;

    bool mIsDraggingSidebar = false;
    bool mIsDraggingImage = false;
    bool mIsDraggingImageButton = false;
    size_t mDraggedImageButtonId;

    Eigen::Vector2f mDraggingStartPosition;

    size_t mClipboardIndex = 0;
};

TEV_NAMESPACE_END
