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
#include <tev/VectorGraphics.h>

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/textbox.h>

#include <chrono>
#include <memory>
#include <set>
#include <vector>

TEV_NAMESPACE_BEGIN

class ImageViewer : public nanogui::Screen {
public:
    ImageViewer(const std::shared_ptr<BackgroundImagesLoader>& imagesLoader, bool fullscreen, bool floatBuffer, bool supportsHdr);
    virtual ~ImageViewer();

    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down, int modifiers) override;
    bool mouse_motion_event(const nanogui::Vector2i& p, const nanogui::Vector2i& rel, int button, int modifiers) override;

    bool drop_event(const std::vector<std::string>& filenames) override;

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    void focusWindow();

    void draw_contents() override;

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);
    void moveImageInList(size_t oldIndex, size_t newIndex);

    bool hasImageWithName(const std::string& imageName) {
        return !!imageByName(imageName);
    }

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) {
        insertImage(image, mImages.size(), shallSelect);
    }

    void removeImage(std::shared_ptr<Image> image);
    void removeImage(const std::string& imageName) {
        removeImage(imageByName(imageName));
    }
    void removeAllImages();

    void replaceImage(std::shared_ptr<Image> image, std::shared_ptr<Image> replacement, bool shallSelect);
    void replaceImage(const std::string& imageName, std::shared_ptr<Image> replacement, bool shallSelect) {
        replaceImage(imageByName(imageName), replacement, shallSelect);
    }

    void reloadImage(std::shared_ptr<Image> image, bool shallSelect = false);
    void reloadImage(const std::string& imageName, bool shallSelect = false) {
        reloadImage(imageByName(imageName), shallSelect);
    }
    void reloadAllImages();
    void reloadImagesWhoseFileChanged();

    void updateImage(
        const std::string& imageName,
        bool shallSelect,
        const std::string& channel,
        int x, int y,
        int width, int height,
        const std::vector<float>& imageData
    );

    void updateImageVectorGraphics(const std::string& imageName, bool shallSelect, bool append, const std::vector<VgCommand>& commands);

    void selectImage(const std::shared_ptr<Image>& image, bool stopPlayback = true);

    void selectGroup(std::string name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() const {
        return mExposureSlider->value();
    }

    void setExposure(float value);

    float offset() const {
        return mOffsetSlider->value();
    }

    void setOffset(float value);

    float gamma() const {
        return mGammaSlider->value();
    }

    void setGamma(float value);

    void normalizeExposureAndOffset();
    void resetImage();

    ETonemap tonemap() const {
        return mImageCanvas->tonemap();
    }

    void setTonemap(ETonemap tonemap);

    EMetric metric() const {
        return mImageCanvas->metric();
    }

    void setMetric(EMetric metric);

    nanogui::Vector2i sizeToFitImage(const std::shared_ptr<Image>& image);
    nanogui::Vector2i sizeToFitAllImages();
    bool setFilter(const std::string& filter);

    bool useRegex() const;
    void setUseRegex(bool value);

    bool watchFilesForChanges() const;
    void setWatchFilesForChanges(bool value);

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

    BackgroundImagesLoader& imagesLoader() const {
        return *mImagesLoader;
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

    bool canDragSidebarFrom(const nanogui::Vector2i& p) {
        return mSidebar->visible() && p.x() - mSidebar->fixed_width() < 10 && p.x() - mSidebar->fixed_width() > -5;
    }

    int visibleSidebarWidth() {
        return mSidebar->visible() ? mSidebar->fixed_width() : 0;
    }

    int visibleFooterHeight() {
        return mFooter->visible() ? mFooter->fixed_height() : 0;
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
    std::set<std::shared_ptr<Image>> mToBump;

    nanogui::TextBox* mFilter;
    nanogui::Button* mRegexButton;

    nanogui::Button* mWatchFilesForChangesButton;
    std::chrono::steady_clock::time_point mLastFileChangesCheck = {};

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

    nanogui::Vector2f mDraggingStartPosition;

    size_t mClipboardIndex = 0;

    bool mSupportsHdr = false;
    nanogui::Button* mClipToLdrButton;

    int mDidFitToImage = 0;
};

TEV_NAMESPACE_END
