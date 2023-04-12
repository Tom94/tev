// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#ifdef _WIN32
#   define NOMINMAX
#   include <winsock2.h>
#   include <Ws2tcpip.h>
#   undef NOMINMAX
#endif

#include <tev/Common.h>
#include <tev/Ipc.h>
#include <tev/ThreadPool.h>

#ifdef _WIN32
using socklen_t = int;
#else
#   include <arpa/inet.h>
#   include <cstring>
#   ifdef EMSCRIPTEN
#       include <fcntl.h>
#   endif
#   include <netdb.h>
#   include <netinet/in.h>
#   include <signal.h>
#   include <sys/file.h>
#   include <sys/socket.h>
#   include <unistd.h>
#   define SOCKET_ERROR (-1)
#   define INVALID_SOCKET (-1)
#endif

using namespace std;

TEV_NAMESPACE_BEGIN

enum SocketError : int {
#ifdef _WIN32
    Again = EAGAIN,
    ConnRefused = WSAECONNREFUSED,
    WouldBlock = WSAEWOULDBLOCK,
#else
    Again = EAGAIN,
    ConnRefused = ECONNREFUSED,
    WouldBlock = EWOULDBLOCK,
#endif
};

IpcPacket::IpcPacket(const char* data, size_t length) {
    if (length <= 0) {
        throw runtime_error{"Cannot construct an IPC packet from no data."};
    }
    mPayload.assign(data, data+length);
}

void IpcPacket::setOpenImage(const string& imagePath, const string& channelSelector, bool grabFocus) {
    OStream payload{mPayload};
    payload << EType::OpenImageV2;
    payload << grabFocus;
    payload << imagePath;
    payload << channelSelector;
}

void IpcPacket::setReloadImage(const string& imageName, bool grabFocus) {
    OStream payload{mPayload};
    payload << EType::ReloadImage;
    payload << grabFocus;
    payload << imageName;
}

void IpcPacket::setCloseImage(const string& imageName) {
    OStream payload{mPayload};
    payload << EType::CloseImage;
    payload << imageName;
}

void IpcPacket::setUpdateImage(const string& imageName, bool grabFocus, const vector<IpcPacket::ChannelDesc>& channelDescs, int32_t x, int32_t y, int32_t width, int32_t height, const vector<float>& stridedImageData) {
    if (channelDescs.empty()) {
        throw runtime_error{"UpdateImage IPC packet must have a non-zero channel count."};
    }

    int32_t nChannels = (int32_t)channelDescs.size();
    vector<string> channelNames(nChannels);
    vector<int64_t> channelOffsets(nChannels);
    vector<int64_t> channelStrides(nChannels);

    for (int32_t i = 0; i < nChannels; ++i) {
        channelNames[i] = channelDescs[i].name;
        channelOffsets[i] = channelDescs[i].offset;
        channelStrides[i] = channelDescs[i].stride;
    }

    OStream payload{mPayload};
    payload << EType::UpdateImageV3;
    payload << grabFocus;
    payload << imageName;
    payload << nChannels;
    payload << channelNames;
    payload << x << y << width << height;
    payload << channelOffsets;
    payload << channelStrides;

    size_t nPixels = width * height;

    size_t stridedImageDataSize = 0;
    for (int32_t c = 0; c < nChannels; ++c) {
        stridedImageDataSize = max(stridedImageDataSize, (size_t)(channelOffsets[c] + (nPixels-1) * channelStrides[c] + 1));
    }

    if (stridedImageData.size() != stridedImageDataSize) {
        throw runtime_error{format("UpdateImage IPC packet's data size does not match specified dimensions, offset, and stride. (Expected: {})", stridedImageDataSize)};
    }

    payload << stridedImageData;
}

void IpcPacket::setCreateImage(const string& imageName, bool grabFocus, int32_t width, int32_t height, int32_t nChannels, const vector<string>& channelNames) {
    if ((int32_t)channelNames.size() != nChannels) {
        throw runtime_error{"CreateImage IPC packet's channel names size does not match number of channels."};
    }

    OStream payload{mPayload};
    payload << EType::CreateImage;
    payload << grabFocus;
    payload << imageName;
    payload << width << height;
    payload << nChannels;
    payload << channelNames;
}

void IpcPacket::setVectorGraphics(const string& imageName, bool grabFocus, bool append, const vector<VgCommand>& commands) {
    OStream payload{mPayload};
    payload << EType::VectorGraphics;
    payload << grabFocus;
    payload << imageName;
    payload << append;
    payload << (int32_t)commands.size();
    for (const auto& command : commands) {
        payload << command.type;
        payload << command.data;
    }
}

IpcPacketOpenImage IpcPacket::interpretAsOpenImage() const {
    IpcPacketOpenImage result;
    IStream payload{mPayload};

    EType type;
    payload >> type;
    if (type != EType::OpenImage && type != EType::OpenImageV2) {
        throw runtime_error{"Cannot interpret IPC packet as OpenImage."};
    }

    payload >> result.grabFocus;

    string imageString;
    payload >> imageString;

    if (type >= EType::OpenImageV2) {
        result.imagePath = imageString;
        payload >> result.channelSelector;
        return result;
    }

    size_t colonPos = imageString.find_last_of(":");
    if (colonPos == string::npos ||
        // windows path of the form X:/* or X:\*
        (colonPos == 1 && imageString.length() >= 3 && (imageString[2] == '\\' || imageString[2] == '/'))
    ) {
        result.imagePath = imageString;
        result.channelSelector = "";
    } else {
        result.imagePath = imageString.substr(0, colonPos);
        result.channelSelector = imageString.substr(colonPos + 1);
    }

    return result;
}

IpcPacketReloadImage IpcPacket::interpretAsReloadImage() const {
    IpcPacketReloadImage result;
    IStream payload{mPayload};

    EType type;
    payload >> type;
    if (type != EType::ReloadImage) {
        throw runtime_error{"Cannot interpret IPC packet as ReloadImage."};
    }

    payload >> result.grabFocus;
    payload >> result.imageName;
    return result;
}

IpcPacketCloseImage IpcPacket::interpretAsCloseImage() const {
    IpcPacketCloseImage result;
    IStream payload{mPayload};

    EType type;
    payload >> type;
    if (type != EType::CloseImage) {
        throw runtime_error{"Cannot interpret IPC packet as CloseImage."};
    }

    payload >> result.imageName;
    return result;
}

IpcPacketUpdateImage IpcPacket::interpretAsUpdateImage() const {
    IpcPacketUpdateImage result;
    IStream payload{mPayload};

    EType type;
    payload >> type;
    if (type != EType::UpdateImage && type != EType::UpdateImageV2 && type != EType::UpdateImageV3) {
        throw runtime_error{"Cannot interpret IPC packet as UpdateImage."};
    }

    payload >> result.grabFocus;
    payload >> result.imageName;

    if (type >= EType::UpdateImageV2) {
        // multi-channel support
        payload >> result.nChannels;
    } else {
        result.nChannels = 1;
    }

    result.channelNames.resize(result.nChannels);
    payload >> result.channelNames;

    result.channelOffsets.resize(result.nChannels);
    result.channelStrides.resize(result.nChannels, 1);

    payload >> result.x >> result.y >> result.width >> result.height;
    size_t nPixels = (size_t)result.width * result.height;

    if (type >= EType::UpdateImageV3) {
        // custom offset/stride support
        payload >> result.channelOffsets;
        payload >> result.channelStrides;
    } else {
        for (int32_t i = 0; i < result.nChannels; ++i) {
            result.channelOffsets[i] = nPixels * i;
        }
    }

    result.imageData.resize(result.nChannels);
    for (int32_t i = 0; i < result.nChannels; ++i) {
        result.imageData[i].resize(nPixels);
    }

    size_t stridedImageDataSize = 0;
    for (int32_t c = 0; c < result.nChannels; ++c) {
        stridedImageDataSize = max(stridedImageDataSize, (size_t)(result.channelOffsets[c] + (nPixels-1) * result.channelStrides[c] + 1));
    }

    if (payload.remainingBytes() < stridedImageDataSize * sizeof(float)) {
        throw runtime_error{"UpdateImage: insufficient image data."};
    }

    const float* stridedImageData = (const float*)payload.get();
    ThreadPool::global().parallelFor<size_t>(0, nPixels, [&](size_t px) {
        for (int32_t c = 0; c < result.nChannels; ++c) {
            result.imageData[c][px] = stridedImageData[result.channelOffsets[c] + px * result.channelStrides[c]];
        }
    }, numeric_limits<int>::max());

    return result;
}

IpcPacketCreateImage IpcPacket::interpretAsCreateImage() const {
    IpcPacketCreateImage result;
    IStream payload{mPayload};

    EType type;
    payload >> type;
    if (type != EType::CreateImage) {
        throw runtime_error{"Cannot interpret IPC packet as CreateImage."};
    }

    payload >> result.grabFocus;
    payload >> result.imageName;
    payload >> result.width >> result.height;
    payload >> result.nChannels;

    result.channelNames.resize(result.nChannels);
    payload >> result.channelNames;

    return result;
}

IpcPacketVectorGraphics IpcPacket::interpretAsVectorGraphics() const {
    IpcPacketVectorGraphics result;
    IStream payload{mPayload};

    EType type;
    payload >> type;
    if (type != EType::VectorGraphics) {
        throw runtime_error{"Cannot interpret IPC packet as VectorGraphics."};
    }

    payload >> result.grabFocus;
    payload >> result.imageName;
    payload >> result.append;
    payload >> result.nCommands;

    result.commands.resize(result.nCommands);
    for (int32_t i = 0; i < result.nCommands; ++i) {
        auto& command = result.commands[i];
        payload >> command.type;
        command.data.resize(command.size());
        payload >> command.data;
    }

    return result;
}

static void makeSocketNonBlocking(Ipc::socket_t socketFd) {
#ifdef _WIN32
    u_long mode = 1;
    int ioctlsocketResult = ioctlsocket(socketFd, FIONBIO, &mode);
    if (ioctlsocketResult != NO_ERROR) {
        throw runtime_error{format("ioctlsocket() to make socket non-blocking failed: {}", errorString(ioctlsocketResult))};
    }
#else
    if (fcntl(socketFd, F_SETFL, fcntl(socketFd, F_GETFL, 0) | O_NONBLOCK) == SOCKET_ERROR) {
        throw runtime_error{format("fcntl() to make socket non-blocking failed: {}", errorString(lastSocketError()))};
    }
#endif
}

static int closeSocket(Ipc::socket_t socket) {
#ifdef _WIN32
    return closesocket(socket);
#else
    return close(socket);
#endif
}

Ipc::Ipc(const string& hostname) : mSocketFd{INVALID_SOCKET} {
    mLockName = ".tev-lock."s + hostname;

    auto parts = split(hostname, ":");
    mIp = parts.front();
    mPort = parts.back();

    try {
        // Networking
#ifdef _WIN32
        // FIXME: only do this once if multiple Ipc objects are created.
        WSADATA wsaData;
        int wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaStartupResult != NO_ERROR) {
            throw runtime_error{format("Could not initialize WSA: {}", errorString(wsaStartupResult))};
        }
#else
        // We don't care about getting a SIGPIPE if the display server goes away...
        signal(SIGPIPE, SIG_IGN);
#endif

        if (attemptToBecomePrimaryInstance()) {
            return;
        }

        // If we're not the primary instance, try to connect to it as a client
        struct addrinfo hints = {}, *addrinfo;
        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        int err = getaddrinfo(mIp.c_str(), mPort.c_str(), &hints, &addrinfo);
        if (err != 0) {
            throw runtime_error{format("getaddrinfo() failed: {}", gai_strerror(err))};
        }

        ScopeGuard addrinfoGuard{[addrinfo] { freeaddrinfo(addrinfo); }};

        mSocketFd = INVALID_SOCKET;
        for (struct addrinfo* ptr = addrinfo; ptr; ptr = ptr->ai_next) {
            mSocketFd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (mSocketFd == INVALID_SOCKET) {
                tlog::warning() << format("socket() failed: {}", errorString(lastSocketError()));
                continue;
            }

            if (connect(mSocketFd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
                int errorId = lastSocketError();
                if (errorId == SocketError::ConnRefused) {
                    throw runtime_error{"Connection to primary instance refused"};
                } else {
                    tlog::warning() << format("connect() failed: {}", errorString(errorId));
                }

                closeSocket(mSocketFd);
                mSocketFd = INVALID_SOCKET;
                continue;
            }

            tlog::success() << "Connected to primary instance " << mIp << ":" << mPort;
            break; // success
        }

        if (mSocketFd == INVALID_SOCKET) {
            throw runtime_error{"Unable to connect to primary instance."};
        }
    } catch (const runtime_error& e) {
        tlog::warning() << "Could not initialize IPC. Assuming primary instance. " << e.what();
        mIsPrimaryInstance = true;
    }
}

Ipc::~Ipc() {
    // Lock
#ifdef _WIN32
    if (mIsPrimaryInstance && mInstanceMutex) {
        ReleaseMutex(mInstanceMutex);
        CloseHandle(mInstanceMutex);
    }
#else
    if (mIsPrimaryInstance) {
        if (mLockFileDescriptor != -1) {
            close(mLockFileDescriptor);
        }

        // Delete the lock file if it exists.
        unlink(mLockFile.string().c_str());
    }
#endif

    // Networking
    if (mSocketFd != INVALID_SOCKET) {
        if (closeSocket(mSocketFd) == SOCKET_ERROR) {
            tlog::warning() << "Error closing socket listen fd " << mSocketFd << ": " << errorString(lastSocketError());
        }
    }

#ifdef _WIN32
    // FIXME: only do this when the last Ipc is destroyed
    WSACleanup();
#endif
}

bool Ipc::attemptToBecomePrimaryInstance() {
#ifdef _WIN32
    // Make sure at most one instance of tev is running
    mInstanceMutex = CreateMutex(NULL, TRUE, mLockName.c_str());

    if (!mInstanceMutex) {
        throw runtime_error{format("Could not obtain global mutex: {}", errorString(lastError()))};
    }

    mIsPrimaryInstance = GetLastError() != ERROR_ALREADY_EXISTS;
    if (!mIsPrimaryInstance) {
        // No need to keep the handle to the existing mutex if we're not the primary instance.
        ReleaseMutex(mInstanceMutex);
        CloseHandle(mInstanceMutex);
    }
#else
    mLockFile = homeDirectory() / mLockName;

    mLockFileDescriptor = open(mLockFile.string().c_str(), O_RDWR | O_CREAT, 0666);
    if (mLockFileDescriptor == -1) {
        throw runtime_error{format("Could not create lock file: {}", errorString(lastError()))};
    }

    mIsPrimaryInstance = !flock(mLockFileDescriptor, LOCK_EX | LOCK_NB);
    if (!mIsPrimaryInstance) {
        close(mLockFileDescriptor);
    }
#endif

    if (!mIsPrimaryInstance) {
        return false;
    }

    // Managed to become primary instance

    // If we were previously a secondary instance connected with the primary instance, disconnect
    if (mSocketFd != INVALID_SOCKET) {
        if (closeSocket(mSocketFd) == SOCKET_ERROR) {
            tlog::warning() << "Error closing socket upon becoming primary instance " << mSocketFd << ": " << errorString(lastSocketError());
        }
    }

    // Set up primary instance network server
    mSocketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (mSocketFd == INVALID_SOCKET) {
        throw runtime_error{format("socket() call failed: {}", errorString(lastSocketError()))};
    }

    makeSocketNonBlocking(mSocketFd);

    // Avoid address in use error that occurs if we quit with a client connected.
    int t = 1;
    if (setsockopt(mSocketFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&t, sizeof(int)) == SOCKET_ERROR) {
        throw runtime_error{format("setsockopt() call failed: {}", errorString(lastSocketError()))};
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(mPort.c_str()));

#ifdef _WIN32
    InetPton(AF_INET, mIp.c_str(), &addr.sin_addr);
#else
    inet_aton(mIp.c_str(), &addr.sin_addr);
#endif

    if (::bind(mSocketFd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        throw runtime_error{format("bind() call failed: {}", errorString(lastSocketError()))};
    }

    if (listen(mSocketFd, 5) == SOCKET_ERROR) {
        throw runtime_error{format("listen() call failed: {}", errorString(lastSocketError()))};
    }

    tlog::success() << "Initialized IPC, listening on " << mIp << ":" << mPort;
    return true;
}

void Ipc::sendToPrimaryInstance(const IpcPacket& message) {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to send to the primary instance."};
    }

    int bytesSent = send(mSocketFd, message.data(), (int)message.size(), 0 /* flags */);
    if (bytesSent != int(message.size())) {
        throw runtime_error{format("send() failed: {}", errorString(lastSocketError()))};
    }
}

void Ipc::receiveFromSecondaryInstance(function<void(const IpcPacket&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    // Check for new connections.
    struct sockaddr_in client;
    socklen_t addrlen = sizeof(client);
    socket_t fd = accept(mSocketFd, (struct sockaddr*)&client, &addrlen);
    if (fd == INVALID_SOCKET) {
        int errorId = lastSocketError();
        if (errorId == SocketError::WouldBlock) {
            // no problem; no one is trying to connect
        } else {
            tlog::warning() << "accept() error: " << errorId << " " << errorString(errorId);
        }
    } else {
        uint32_t ip = ntohl(client.sin_addr.s_addr);
        uint16_t port = ntohs(client.sin_port);
        auto name = format("{}.{}.{}.{}:{}", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, port);
        tlog::info() << format("Client {} (#{}) connected", name, fd);
        mSocketConnections.push_back(SocketConnection{fd, name});
    }

    // Service existing connections.
    for (auto iter = mSocketConnections.begin(); iter != mSocketConnections.end();) {
        auto cur = iter++;
        cur->service(callback);

        // If the connection became closed, stop keeping track of it.
        if (cur->isClosed()) {
            mSocketConnections.erase(cur);
        }
    }
}

Ipc::SocketConnection::SocketConnection(Ipc::socket_t fd, const string& name)
: mSocketFd{fd}, mName{name}
{
    TEV_ASSERT(mSocketFd != INVALID_SOCKET, "SocketConnection must receive a valid socket.");

    makeSocketNonBlocking(mSocketFd);

    // 1 MiB is a good default buffer size. If larger is required, it'll be automatizally resized.
    mBuffer.resize(1024 * 1024);
}

void Ipc::SocketConnection::service(function<void(const IpcPacket&)> callback) {
    if (isClosed()) {
        // Client disconnected, so don't bother.
        return;
    }

    while (true) {
        // Receive as much data as we can, up to the capacity of 'mBuffer'.
        size_t maxBytes = mBuffer.size() - mRecvOffset;
        int bytesReceived = recv(mSocketFd, mBuffer.data() + mRecvOffset, (int)maxBytes, 0);
        if (bytesReceived == SOCKET_ERROR) {
            int errorId = lastSocketError();
            // no more data; this is fine.
            if (errorId == SocketError::Again || errorId == SocketError::WouldBlock) {
                break;
            } else {
                tlog::warning() << "Error while reading from socket. " << errorString(errorId) << " Connection terminated.";
                close();
                return;
            }
        }

        TEV_ASSERT(bytesReceived >= 0, "With no error, the number of bytes received should be positive.");
        mRecvOffset += (size_t)bytesReceived;

        // Since we aren't getting annoying SIGPIPE signals when a client
        // disconnects, a zero-byte read here is how we know when that happens.
        if (bytesReceived == 0) {
            tlog::info() << "Client " << mName << " (#" << mSocketFd << ") disconnected";
            close();
            return;
        }

        // Go through the buffer and service as many complete messages as
        // we can find.
        size_t processedOffset = 0;
        while (processedOffset + 4 <= mRecvOffset) {
            // There's at least enough to figure out the next message's length.
            const char* messagePtr = mBuffer.data() + processedOffset;
            uint32_t messageLength = *((uint32_t*)messagePtr);

            if (messageLength > mBuffer.size()) {
                mBuffer.resize(messageLength);
                break;
            }

            if (processedOffset + messageLength <= mRecvOffset) {
                // We have a full message.
                callback(IpcPacket{messagePtr, messageLength});
                processedOffset += messageLength;
            } else {
                // It's a partial message; we'll need to recv() more.
                break;
            }
        }

        // TODO: we could save the memcpy by treating 'buffer' as a ring-buffer,
        // but it's probably not worth the trouble. Revisit when someone throws around
        // buffers with a size of gigabytes.
        if (processedOffset > 0) {
            // There's a partial message; copy it to the start of 'buffer'
            // and update the offsets accordingly.
            memmove(mBuffer.data(), mBuffer.data() + processedOffset, mRecvOffset - processedOffset);
            mRecvOffset -= processedOffset;
        }
    }
}

void Ipc::SocketConnection::close() {
    if (!isClosed()) {
        closeSocket(mSocketFd);
        mSocketFd = INVALID_SOCKET;
    }
}

bool Ipc::SocketConnection::isClosed() const {
    return mSocketFd == INVALID_SOCKET;
}

TEV_NAMESPACE_END
