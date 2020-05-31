// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Common.h>
#include <tev/Ipc.h>

#ifdef _WIN32
using socklen_t = int;
#else
#   include <arpa/inet.h>
#   include <cstring>
#   ifdef EMSCRIPTEN
#       include <fcntl.h>
#   endif
#   include <sys/file.h>
#   include <unistd.h>
#   define SOCKET_ERROR (-1)
#   define INVALID_SOCKET (-1)
#endif

using namespace std;

TEV_NAMESPACE_BEGIN

IpcPacket::IpcPacket(const char* data, size_t length) {
    if (length <= 0) {
        throw runtime_error{"Cannot construct an IPC packet from no data."};
    }
    mPayload.assign(data, data+length);
}

void IpcPacket::setOpenImage(const string& imagePath, bool grabFocus) {
    OStream payload{mPayload};
    payload << Type::OpenImage;
    payload << grabFocus;
    payload << imagePath;
}

void IpcPacket::setReloadImage(const string& imagePath, bool grabFocus) {
    OStream payload{mPayload};
    payload << Type::ReloadImage;
    payload << grabFocus;
    payload << imagePath;
}

void IpcPacket::setCloseImage(const string& imagePath) {
    OStream payload{mPayload};
    payload << Type::CloseImage;
    payload << imagePath;
}

void IpcPacket::setUpdateImage(const string& imagePath, bool grabFocus, const string& channel, int x, int y, int width, int height, const vector<float>& imageData) {
    if ((int)imageData.size() != width * height) {
        throw runtime_error{"UpdateImage IPC packet's data size does not match crop windows."};
    }

    OStream payload{mPayload};
    payload << Type::UpdateImage;
    payload << grabFocus;
    payload << imagePath;
    payload << channel;
    payload << x << y << width << height;
    payload << imageData;
}

IpcPacketOpenImage IpcPacket::interpretAsOpenImage() const {
    IpcPacketOpenImage result;
    IStream payload{mPayload};

    Type type;
    payload >> type;
    if (type != Type::OpenImage) {
        throw runtime_error{"Cannot interpret IPC packet as OpenImage."};
    }

    payload >> result.grabFocus;
    payload >> result.imagePath;
    return result;
}

IpcPacketReloadImage IpcPacket::interpretAsReloadImage() const {
    IpcPacketReloadImage result;
    IStream payload{mPayload};

    Type type;
    payload >> type;
    if (type != Type::ReloadImage) {
        throw runtime_error{"Cannot interpret IPC packet as ReloadImage."};
    }

    payload >> result.grabFocus;
    payload >> result.imagePath;
    return result;
}

IpcPacketCloseImage IpcPacket::interpretAsCloseImage() const {
    IpcPacketCloseImage result;
    IStream payload{mPayload};

    Type type;
    payload >> type;
    if (type != Type::CloseImage) {
        throw runtime_error{"Cannot interpret IPC packet as CloseImage."};
    }

    payload >> result.imagePath;
    return result;
}

IpcPacketUpdateImage IpcPacket::interpretAsUpdateImage() const {
    IpcPacketUpdateImage result;
    IStream payload{mPayload};

    Type type;
    payload >> type;
    if (type != Type::UpdateImage) {
        throw runtime_error{"Cannot interpret IPC packet as UpdateImage."};
    }

    payload >> result.grabFocus;
    payload >> result.imagePath;
    payload >> result.channel;
    payload >> result.x >> result.y >> result.width >> result.height;

    size_t nPixels = result.width * result.height;

    // This is a very conservative upper bound on how many floats
    // fit into a UDP packet
    if (nPixels > 100000) {
        throw runtime_error{"Too many pixels in UpdateImage IPC packet."};
    }

    result.imageData.resize(nPixels);
    payload >> result.imageData;
    return result;
}


Ipc::Ipc(const string& hostname) {
    const string lockName = string{".tev-lock."} + hostname;

    auto parts = split(hostname, ":");
    const string& ip = parts.front();
    short port = (short)atoi(parts.back().c_str());

    memset((char*)&mAddress, 0, sizeof(mAddress));

    try {
#ifdef _WIN32
        //Make sure at most one instance of the tool is running
        mInstanceMutex = CreateMutex(NULL, TRUE, lockName.c_str());

        if (!mInstanceMutex) {
            throw runtime_error{tfm::format("Could not obtain global mutex: %s", errorString(lastError()))};
        }

        mIsPrimaryInstance = GetLastError() != ERROR_ALREADY_EXISTS;
        if (!mIsPrimaryInstance) {
            // No need to keep the handle to the existing mutex if we're not the primary instance.
            ReleaseMutex(mInstanceMutex);
            CloseHandle(mInstanceMutex);
        }

        // Initialize Winsock
        WSADATA wsaData;
        int wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaStartupResult != NO_ERROR) {
            throw runtime_error{tfm::format("Could not initialize WSA: %s", errorString(wsaStartupResult))};
        }
#else
        mLockFile = homeDirectory() / lockName;

        mLockFileDescriptor = open(mLockFile.str().c_str(), O_RDWR | O_CREAT, 0666);
        if (mLockFileDescriptor == -1) {
            throw runtime_error{tfm::format("Could not create lock file: ", errorString(lastError()))};
        }

        mIsPrimaryInstance = !flock(mLockFileDescriptor, LOCK_EX | LOCK_NB);
        if (!mIsPrimaryInstance) {
            close(mLockFileDescriptor);
        }
#endif

        mAddress.sin_family = AF_INET;
        mAddress.sin_port = htons(port);

#ifdef _WIN32
        mAddress.sin_addr.S_un.S_addr = inet_addr(hostname.c_str());
#else
        inet_aton(hostname.c_str(), &mAddress.sin_addr);
#endif

        mSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mSocket == INVALID_SOCKET) {
            throw runtime_error{tfm::format("Could not create UDP socket: ", errorString(lastSocketError()))};
        }

        // Make socket non-blocking
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(mSocket, FIONBIO, &mode);
#else
        int flags = fcntl(mSocket, F_GETFL, 0);
        fcntl(mSocket, F_SETFL, flags | O_NONBLOCK);
#endif

        // Listen to UDP packets if we're the primary instance.
        if (mIsPrimaryInstance) {
            int result = ::bind(mSocket, (sockaddr*)&mAddress, sizeof(mAddress));
            if (result == SOCKET_ERROR) {
                throw runtime_error{tfm::format("Could not bind UDP socket: %s", errorString(lastSocketError()))};
            }
        }
    } catch (runtime_error e) {
        tlog::warning() << "Could not initialize IPC; assuming primary instance. " << e.what();
        mIsPrimaryInstance = true;
    }

    tlog::success() << "Initialized IPC, listening on " << ip << ":" << port;
}

Ipc::~Ipc() {
#ifdef _WIN32
    if (mIsPrimaryInstance && mInstanceMutex) {
        ReleaseMutex(mInstanceMutex);
        CloseHandle(mInstanceMutex);
    }

    if (mSocket != INVALID_SOCKET) {
        closesocket(mSocket);
    }
#else
    if (mIsPrimaryInstance) {
        if (mLockFileDescriptor != -1) {
            close(mLockFileDescriptor);
        }

        // Delete the lock file if it exists.
        unlink(mLockFile.str().c_str());
    }

    if (mSocket != INVALID_SOCKET) {
        close(mSocket);
    }
#endif
}

void Ipc::sendToPrimaryInstance(const IpcPacket& message) {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to send to the primary instance."};
    }

    int bytesSent = sendto(mSocket, message.data(), (int)message.size(), 0, (sockaddr*)&mAddress, sizeof(mAddress));
    if (bytesSent == -1) {
        throw runtime_error{tfm::format("Could not send to primary instance: %s", errorString(lastSocketError()))};
    }
}

bool Ipc::receiveFromSecondaryInstance(function<void(const IpcPacket&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    sockaddr_in recvAddress;
    socklen_t recvAddressSize = sizeof(recvAddress);
    char recvBuffer[8192];

    int bytesReceived = recvfrom(mSocket, recvBuffer, sizeof(recvBuffer), 0, (sockaddr*)&recvAddress, &recvAddressSize);
    if (bytesReceived <= 0) {
        return false;
    }

    if (bytesReceived >= (int)sizeof(recvBuffer)) {
        tlog::warning() << "IPC packet was too big (>=" << sizeof(recvBuffer) << ")";
        return false;
    }

    IpcPacket packet{recvBuffer, (size_t)bytesReceived};
    callback(packet);
    return true;
}

TEV_NAMESPACE_END
