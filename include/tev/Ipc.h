// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <filesystem/path.h>

#include <list>
#include <vector>

TEV_NAMESPACE_BEGIN

struct IpcPacketOpenImage {
    std::string imagePath;
    bool grabFocus;
};

struct IpcPacketReloadImage {
    std::string imageName;
    bool grabFocus;
};

struct IpcPacketUpdateImage {
    std::string imageName;
    bool grabFocus;
    std::string channel;
    int32_t x, y, width, height;
    std::vector<float> imageData;
};

struct IpcPacketCloseImage {
    std::string imageName;
};

struct IpcPacketCreateImage {
    std::string imageName;
    bool grabFocus;
    int32_t width, height;
    int32_t nChannels;
    std::vector<std::string> channelNames;
};

class IpcPacket {
public:
    enum Type : char {
        OpenImage = 0,
        ReloadImage = 1,
        CloseImage = 2,
        UpdateImage = 3,
        CreateImage = 4,
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
        // The first 4 bytes encode the message size.
        return (Type)mPayload[4];
    }

    void setOpenImage(const std::string& imagePath, bool grabFocus);
    void setReloadImage(const std::string& imageName, bool grabFocus);
    void setCloseImage(const std::string& imageName);
    void setUpdateImage(const std::string& imageName, bool grabFocus, const std::string& channel, int x, int y, int width, int height, const std::vector<float>& imageData);
    void setCreateImage(const std::string& imageName, bool grabFocus, int width, int height, int nChannels, const std::vector<std::string>& channelNames);

    IpcPacketOpenImage interpretAsOpenImage() const;
    IpcPacketReloadImage interpretAsReloadImage() const;
    IpcPacketCloseImage interpretAsCloseImage() const;
    IpcPacketUpdateImage interpretAsUpdateImage() const;
    IpcPacketCreateImage interpretAsCreateImage() const;

private:
    std::vector<char> mPayload;

    class IStream {
    public:
        IStream(const std::vector<char>& data) : mData{data} {
            uint32_t size;
            *this >> size;
            if ((size_t)size != data.size()) {
                throw std::runtime_error{"Trying to read IPC packet with incorrect size."};
            }
        }

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
        OStream(std::vector<char>& data) : mData{data} {
            // Reserve space for an integer denoting the size
            // of the packet.
            *this << (uint32_t)0;
        }

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
            updateSize();
            return *this;
        }

        template <typename T>
        OStream& operator<<(T var) {
            if (mData.size() < mIdx + sizeof(T)) {
                mData.resize(mIdx + sizeof(T));
            }

            *(T*)&mData[mIdx] = var;
            mIdx += sizeof(T);
            updateSize();
            return *this;
        }
    private:
        void updateSize() {
            *((uint32_t*)mData.data()) = (uint32_t)mIdx;
        }

        std::vector<char>& mData;
        size_t mIdx = 0;
    };
};

class Ipc {
public:
#ifdef _WIN32
    using socket_t = SOCKET;
#else
    using socket_t = int;
#endif

    Ipc(const std::string& hostname);
    virtual ~Ipc();

    bool isPrimaryInstance() {
        return mIsPrimaryInstance;
    }

    void sendToPrimaryInstance(const IpcPacket& message);
    void receiveFromSecondaryInstance(std::function<void(const IpcPacket&)> callback);

private:
    bool mIsPrimaryInstance;
    socket_t mSocketFd;

#ifdef _WIN32
    HANDLE mInstanceMutex;
#else
    int mLockFileDescriptor;
    filesystem::path mLockFile;
#endif

    class SocketConnection {
    public:
        SocketConnection(Ipc::socket_t fd);

        void service(std::function<void(const IpcPacket&)> callback);

        void close();

        bool isClosed() const;

    private:
        Ipc::socket_t mSocketFd;

        // Because TCP socket recv() calls return as much data as is available
        // (which may have the partial contents of a client-side send() call,
        // we need to buffer it up in SocketConnection.
        std::vector<char> mBuffer;
        // Offset into buffer where next recv() call should start writing.
        size_t mRecvOffset = 0;
    };

    std::list<SocketConnection> mSocketConnections;
};

TEV_NAMESPACE_END
