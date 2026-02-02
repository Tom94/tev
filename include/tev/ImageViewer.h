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

#pragma once

#include <tev/HelpWindow.h>
#include <tev/Image.h>
#include <tev/ImageButton.h>
#include <tev/ImageCanvas.h>
#include <tev/ImageInfoWindow.h>
#include <tev/Ipc.h>
#include <tev/Lazy.h>
#include <tev/MultiGraph.h>
#include <tev/SharedQueue.h>
#include <tev/VectorGraphics.h>
#include <tev/imageio/Colors.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/textbox.h>

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace tev {

class ImageViewer : public nanogui::Screen {
public:
    ImageViewer(
        const nanogui::Vector2i& size,
        const std::shared_ptr<BackgroundImagesLoader>& imagesLoader,
        std::weak_ptr<Ipc> ipc,
        bool maximize,
        bool showUi,
        bool floatBuffer
    );

    bool resize_event(const nanogui::Vector2i& size) override;

    bool mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) override;
    bool mouse_motion_event_f(const nanogui::Vector2f& p, const nanogui::Vector2f& rel, int button, int modifiers) override;

    bool drop_event(const std::vector<std::string>& filenames) override;

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    void focusWindow();

    void draw_contents() override;

    void updateColorCapabilities();

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);
    void moveImageInList(size_t oldIndex, size_t newIndex);

    bool hasImageWithName(std::string_view imageName) { return !!imageByName(imageName); }

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) { insertImage(image, mImages.size(), shallSelect); }

    void removeImage(std::shared_ptr<Image> image);
    void removeImage(std::string_view imageName) { removeImage(imageByName(imageName)); }
    void removeAllImages();

    void replaceImage(std::shared_ptr<Image> image, std::shared_ptr<Image> replacement, bool shallSelect);
    void replaceImage(std::string_view imageName, std::shared_ptr<Image> replacement, bool shallSelect) {
        replaceImage(imageByName(imageName), replacement, shallSelect);
    }

    void reloadImage(std::shared_ptr<Image> image, bool shallSelect = false);
    void reloadImage(std::string_view imageName, bool shallSelect = false) { reloadImage(imageByName(imageName), shallSelect); }
    void reloadAllImages();
    void reloadImagesWhoseFileChanged();

    void updateImage(
        std::string_view imageName, bool shallSelect, std::string_view channel, int x, int y, int width, int height, std::span<const float> imageData
    );

    void updateImageVectorGraphics(std::string_view imageName, bool shallSelect, bool append, std::span<const VgCommand> commands);

    void selectImage(const std::shared_ptr<Image>& image, bool stopPlayback = true);

    void selectGroup(std::string name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() const { return mExposureSlider->value(); }

    void setExposure(float value);

    float offset() const { return mOffsetSlider->value(); }

    void setOffset(float value);

    float gamma() const { return mGammaSlider->value(); }

    void setGamma(float value);

    void normalizeExposureAndOffset();

    void resetImage();

    EInterpolationMode minFilter() const { return mImageCanvas->minFilter(); }
    void setMinFilter(EInterpolationMode value) { mImageCanvas->setMinFilter(value); }

    EInterpolationMode magFilter() const { return mImageCanvas->magFilter(); }
    void setMagFilter(EInterpolationMode value) { mImageCanvas->setMagFilter(value); }

    ETonemap tonemap() const { return mImageCanvas->tonemap(); }
    void setTonemap(ETonemap tonemap);

    EMetric metric() const { return mImageCanvas->metric(); }
    void setMetric(EMetric metric);

    void setBackgroundColorStraight(const nanogui::Color& color);

    float displayWhiteLevel() const;
    void setDisplayWhiteLevel(float value);
    void setDisplayWhiteLevelToImageMetadata();
    void setImageWhiteLevel(float value);

    enum class EDisplayWhiteLevelSetting {
        System = 0,
        Custom = 1,
        ImageMetadata = 2,
    };

    EDisplayWhiteLevelSetting displayWhiteLevelSetting() const;
    void setDisplayWhiteLevelSetting(EDisplayWhiteLevelSetting value);

    nanogui::Vector2f sizeToFitImage(const std::shared_ptr<Image>& image);
    nanogui::Vector2f sizeToFitAllImages();
    void resizeToFit(nanogui::Vector2f size);

    bool playingBack() const;
    void setPlayingBack(bool value);

    bool setFilter(std::string_view filter);

    void setFps(int value);

    bool useRegex() const;
    void setUseRegex(bool value);

    bool watchFilesForChanges() const;
    void setWatchFilesForChanges(bool value);

    bool autoFitToScreen() const;
    void setAutoFitToScreen(bool value);

    bool resizeWindowToFitImageOnLoad() const;
    void setResizeWindowToFitImageOnLoad(bool value);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() { return mSidebar->visible(); }
    void setUiVisible(bool shouldBeVisible);

    void toggleHelpWindow();

    void toggleImageInfoWindow();
    void updateImageInfoWindow();

    void openImageDialog();
    void saveImageDialog();

    void requestLayoutUpdate() { mRequiresLayoutUpdate = true; }

    template <typename T> void scheduleToUiThread(const T& fun) {
        mTaskQueue.push(fun);
        redraw();
    }

    BackgroundImagesLoader& imagesLoader() const { return *mImagesLoader; }
    std::weak_ptr<Ipc> ipc() const { return mIpc; }

    void copyImageCanvasToClipboard() const;
    void copyImageNameToClipboard() const;
    void pasteImagesFromClipboard();

    void showErrorDialog(std::string_view message);

    chroma_t inspectionChroma() const;
    void setInspectionChroma(const chroma_t& chroma);

    ituth273::ETransfer inspectionTransfer() const;
    void setInspectionTransfer(ituth273::ETransfer transfer);

    bool inspectionAdaptWhitePoint() const;
    void setInspectionAdaptWhitePoint(bool adapt);

    bool inspectionPremultipliedAlpha() const;
    void setInspectionPremultipliedAlpha(bool value);

private:
    void updateFilter();
    void updateLayout();
    void updateTitle();
    std::string groupName(size_t index);

    int groupId(std::string_view groupName) const;
    int imageId(const std::shared_ptr<Image>& image) const;
    int imageId(std::string_view imageName) const;

    std::string nextGroup(std::string_view groupName, EDirection direction);
    std::string nthVisibleGroup(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image, EDirection direction);
    std::shared_ptr<Image> nthVisibleImage(size_t n);
    std::shared_ptr<Image> imageByName(std::string_view imageName);

    bool canDragSidebarFrom(const nanogui::Vector2i& p) {
        return mSidebar->visible() && p.x() - mSidebar->fixed_width() < 10 && p.x() - mSidebar->fixed_width() > -5;
    }

    int visibleSidebarWidth() { return mSidebar->visible() ? mSidebar->fixed_width() : 0; }

    int visibleFooterHeight() { return mFooter->visible() ? mFooter->fixed_height() : 0; }

    void updateCurrentMonitorSize();

    SharedQueue<std::function<void(void)>> mTaskQueue;

    bool mRequiresFilterUpdate = true;
    bool mRequiresLayoutUpdate = true;

    nanogui::Widget* mVerticalScreenSplit = nullptr;

    nanogui::Widget* mSidebar = nullptr;
    nanogui::Button* mHelpButton = nullptr;
    nanogui::Widget* mSidebarLayout = nullptr;

    nanogui::Widget* mFooter = nullptr;
    bool mShouldFooterBeVisible = false;

    nanogui::Label* mExposureLabel = nullptr;
    nanogui::Slider* mExposureSlider = nullptr;

    nanogui::Label* mOffsetLabel = nullptr;
    nanogui::Slider* mOffsetSlider = nullptr;

    nanogui::Label* mGammaLabel = nullptr;
    nanogui::Slider* mGammaSlider = nullptr;

    nanogui::Widget* mTonemapButtonContainer = nullptr;
    nanogui::Widget* mMetricButtonContainer = nullptr;

    std::shared_ptr<BackgroundImagesLoader> mImagesLoader;
    std::weak_ptr<Ipc> mIpc;

    std::shared_ptr<Image> mCurrentImage;
    std::shared_ptr<Image> mCurrentReference;

    std::vector<std::shared_ptr<Image>> mImages;

    MultiGraph* mHistogram = nullptr;
    std::set<std::shared_ptr<Image>> mToBump;

    nanogui::TextBox* mFilter = nullptr;
    nanogui::Button* mRegexButton = nullptr;

    nanogui::Button* mWatchFilesForChangesButton = nullptr;
    std::chrono::steady_clock::time_point mLastFileChangesCheckTime = {};

    nanogui::Button* mAutoFitToScreenButton = nullptr;
    nanogui::Button* mResizeWindowToFitImageOnLoadButton = nullptr;

    // Buttons which require a current image to be meaningful.
    std::vector<nanogui::Button*> mCurrentImageButtons;
    nanogui::Button* mImageInfoButton = nullptr;
    ImageInfoWindow* mImageInfoWindow = nullptr;

    // Buttons which require at least one image to be meaningful
    std::vector<nanogui::Button*> mAnyImageButtons;

    nanogui::Button* mPlayButton = nullptr;
    nanogui::IntBox<int>* mFpsTextBox = nullptr;
    std::chrono::steady_clock::time_point mLastPlaybackFrameTime = {};

    nanogui::Widget* mImageButtonContainer = nullptr;
    nanogui::Widget* mScrollContent = nullptr;
    nanogui::VScrollPanel* mImageScrollContainer = nullptr;

    ImageCanvas* mImageCanvas = nullptr;

    nanogui::Widget* mGroupButtonContainer = nullptr;
    std::string mCurrentGroup;

    HelpWindow* mHelpWindow = nullptr;

    enum class EMouseDragType {
        None,
        ImageDrag,
        ImageCrop,
        ImageButtonDrag,
        SidebarDrag,
    };

    nanogui::Vector2i mDraggingStartPosition;
    EMouseDragType mDragType = EMouseDragType::None;
    size_t mDraggedImageButtonId;

    size_t mClipboardIndex = 0;

    // HDR UI elements support
    struct ColorSpace {
        ituth273::ETransfer transfer;
        EWpPrimaries primaries;
        float maxLuminance;

        bool operator==(const ColorSpace& other) const {
            return transfer == other.transfer && primaries == other.primaries && maxLuminance == other.maxLuminance;
        }
    };

    std::optional<ColorSpace> mSystemColorSpace;

    nanogui::PopupButton* mColorsPopupButton = nullptr;

    nanogui::ComboBox* mInspectionPrimariesComboBox = nullptr;
    std::vector<nanogui::FloatBox<float>*> mInspectionPrimariesBoxes;
    nanogui::ComboBox* mInspectionTransferComboBox = nullptr;
    nanogui::Button* mInspectionAdaptWhitePointButton = nullptr;
    nanogui::Button* mInspectionPremultipliedAlphaButton = nullptr;

    nanogui::ColorWheel* mBackgroundColorWheel = nullptr;
    nanogui::Slider* mBackgroundAlphaSlider = nullptr;

    nanogui::PopupButton* mHdrPopupButton = nullptr;
    nanogui::Button* mClipToLdrButton = nullptr;

    nanogui::FloatBox<float>* mDisplayWhiteLevelBox = nullptr;
    nanogui::ComboBox* mDisplayWhiteLevelSettingComboBox = nullptr;

    nanogui::FloatBox<float>* mImageWhiteLevelBox = nullptr;

    // Misc state tracking variables
    int mDidFitToImage = 0;

    nanogui::Vector2f mMinWindowPos = {0, 0};
    nanogui::Vector2f mMaxWindowSize = {8192, 8192};

    bool mInitialized = false;

    bool mMaximizedLaunch = false;
    bool mMaximizedUnreliable = false;

    std::unique_ptr<std::thread> mFileDialogThread;
};

} // namespace tev
