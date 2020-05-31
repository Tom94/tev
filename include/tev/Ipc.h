// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <filesystem/path.h>

#include <enet/enet.h>

#include <vector>

TEV_NAMESPACE_BEGIN

struct IpcPacketOpenImage {
    std::string imagePath;
    bool grabFocus;
};

struct IpcPacketReloadImage {
    std::string imagePath;
    bool grabFocus;
};

struct IpcPacketUpdateImage {
    std::string imagePath;
    bool grabFocus;
    std::string channel;
    int32_t x, y, width, height;
    std::vector<float> imageData;
};

struct IpcPacketCloseImage {
    std::string imagePath;
};

class IpcPacket {
public:
    enum Type : char {
        OpenImage = 0,
        ReloadImage = 1,
        CloseImage = 2,
        UpdateImage = 3,
    };

    IpcPacket() = default;
    IpcPacket(const char* data, size_t length);

    const char* data() const {
        return mPayload.data();
    }

    size_t size() const {
        return mPayload.size();
    }

    Type type() const {
        return (Type)mPayload[0];
    }

    void setOpenImage(const std::string& imagePath, bool grabFocus);
    void setReloadImage(const std::string& imagePath, bool grabFocus);
    void setCloseImage(const std::string& imagePath);
    void setUpdateImage(const std::string& imagePath, bool grabFocus, const std::string& channel, int x, int y, int width, int height, const std::vector<float>& imageData);

    IpcPacketOpenImage interpretAsOpenImage() const;
    IpcPacketReloadImage interpretAsReloadImage() const;
    IpcPacketCloseImage interpretAsCloseImage() const;
    IpcPacketUpdateImage interpretAsUpdateImage() const;

private:
    std::vector<char> mPayload;

    class IStream {
    public:
        IStream(const std::vector<char>& data) : mData{data} {}

        IStream& operator>>(bool& var) {
            if (mData.size() < mIdx + 1) {
                throw std::runtime_error{"Trying to read beyond the bounds of the IPC packet payload."};
            }

            var = mData[mIdx] == 1;
            ++mIdx;
            return *this;
        }

        IStream& operator>>(std::string& var) {
            std::vector<char> buffer;
            do {
                buffer.push_back(mData[mIdx]);
            } while (mData[mIdx++] != '\0');
            var = buffer.data();
            return *this;
        }

        template <typename T>
        IStream& operator>>(std::vector<T>& var) {
            for (auto& elem : var) {
                *this >> elem;
            }
            return *this;
        }

        template <typename T>
        IStream& operator>>(T& var) {
            if (mData.size() < mIdx + sizeof(T)) {
                throw std::runtime_error{"Trying to read beyond the bounds of the IPC packet payload."};
            }

            var = *(T*)&mData[mIdx];
            mIdx += sizeof(T);
            return *this;
        }
    private:
        const std::vector<char>& mData;
        size_t mIdx = 0;
    };

    class OStream {
    public:
        OStream(std::vector<char>& data) : mData{data} {}

        template <typename T>
        OStream& operator<<(const std::vector<T>& var) {
            for (auto&& elem : var) {
                *this << elem;
            }
            return *this;
        }

        OStream& operator<<(const std::string& var) {
            for (auto&& character : var) {
                *this << character;
            }
            *this << '\0';
            return *this;
        }

        OStream& operator<<(bool var) {
            if (mData.size() < mIdx + 1) {
                mData.resize(mIdx + 1);
            }

            mData[mIdx] = var ? 1 : 0;
            ++mIdx;
            return *this;
        }

        template <typename T>
        OStream& operator<<(T var) {
            if (mData.size() < mIdx + sizeof(T)) {
                mData.resize(mIdx + sizeof(T));
            }

            *(T*)&mData[mIdx] = var;
            mIdx += sizeof(T);
            return *this;
        }
    private:
        std::vector<char>& mData;
        size_t mIdx = 0;
    };
};

class Ipc {
public:
    Ipc(const std::string& hostname);
    virtual ~Ipc();

    bool isPrimaryInstance() {
        return mIsPrimaryInstance;
    }

    void sendToPrimaryInstance(const IpcPacket& message);
    void receiveFromSecondaryInstance(std::function<void(const IpcPacket&)> callback);

private:
    bool mIsPrimaryInstance;
    
    ENetAddress mAddress;
    ENetHost* mSocket = nullptr;

    // Represents the server when this is a
    // secondary instance of tev.
    ENetPeer* mPeer = nullptr;

#ifdef _WIN32
    HANDLE mInstanceMutex;
#else
    int mLockFileDescriptor;
    filesystem::path mLockFile;
#endif
};

TEV_NAMESPACE_END
