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

void IpcPacket::setOpenImage(const string& imagePath, bool grabFocus) {
    OStream payload{mPayload};
    payload << Type::OpenImage;
    payload << grabFocus;
    payload << imagePath;
}

void IpcPacket::setReloadImage(const string& imageName, bool grabFocus) {
    OStream payload{mPayload};
    payload << Type::ReloadImage;
    payload << grabFocus;
    payload << imageName;
}

void IpcPacket::setCloseImage(const string& imageName) {
    OStream payload{mPayload};
    payload << Type::CloseImage;
    payload << imageName;
}

void IpcPacket::setUpdateImage(const string& imageName, bool grabFocus, const string& channel, int x, int y, int width, int height, const vector<float>& imageData) {
    if ((int)imageData.size() != width * height) {
        throw runtime_error{"UpdateImage IPC packet's data size does not match crop windows."};
    }

    OStream payload{mPayload};
    payload << Type::UpdateImage;
    payload << grabFocus;
    payload << imageName;
    payload << channel;
    payload << x << y << width << height;
    payload << imageData;
}

void IpcPacket::setCreateImage(const std::string& imageName, bool grabFocus, int width, int height, int nChannels, const std::vector<std::string>& channelNames) {
    if ((int)channelNames.size() != nChannels) {
        throw runtime_error{"CreateImage IPC packet's channel names size does not match number of channels."};
    }

    OStream payload{mPayload};
    payload << Type::CreateImage;
    payload << grabFocus;
    payload << imageName;
    payload << width << height;
    payload << nChannels;
    payload << channelNames;
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
    payload >> result.imageName;
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

    payload >> result.imageName;
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
    payload >> result.imageName;
    payload >> result.channel;
    payload >> result.x >> result.y >> result.width >> result.height;

    size_t nPixels = result.width * result.height;
    result.imageData.resize(nPixels);
    payload >> result.imageData;
    return result;
}

IpcPacketCreateImage IpcPacket::interpretAsCreateImage() const {
    IpcPacketCreateImage result;
    IStream payload{mPayload};

    Type type;
    payload >> type;
    if (type != Type::CreateImage) {
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


static void makeSocketNonBlocking(Ipc::socket_t socketFd) {
#ifdef _WIN32
    u_long mode = 1;
    int ioctlsocketResult = ioctlsocket(socketFd, FIONBIO, &mode);
    if (ioctlsocketResult != NO_ERROR) {
        throw runtime_error{tfm::format("ioctlsocket() to make socket non-blocking failed: %s", errorString(ioctlsocketResult))};
    }
#else
    if (fcntl(socketFd, F_SETFL, fcntl(socketFd, F_GETFL, 0) | O_NONBLOCK) == SOCKET_ERROR) {
        throw runtime_error{tfm::format("fcntl() to make socket non-blocking failed: %s", errorString(lastSocketError()))};
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

Ipc::Ipc(const string& hostname) {
    const string lockName = string{".tev-lock."} + hostname;

    auto parts = split(hostname, ":");
    const string& ip = parts.front();
    const string& port = parts.back();

    try {
        // Lock file
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
#else
        mLockFile = homeDirectory() / lockName;

        mLockFileDescriptor = open(mLockFile.str().c_str(), O_RDWR | O_CREAT, 0666);
        if (mLockFileDescriptor == -1) {
            throw runtime_error{tfm::format("Could not create lock file: %s", errorString(lastError()))};
        }

        mIsPrimaryInstance = !flock(mLockFileDescriptor, LOCK_EX | LOCK_NB);
        if (!mIsPrimaryInstance) {
            close(mLockFileDescriptor);
        }
#endif

        // Networking
#ifdef _WIN32
        // FIXME: only do this once if multiple Ipc objects are created.
        WSADATA wsaData;
        int wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaStartupResult != NO_ERROR) {
            throw runtime_error{tfm::format("Could not initialize WSA: %s", errorString(wsaStartupResult))};
        }
#else
        // We don't care about getting a SIGPIPE if the display server goes away...
        signal(SIGPIPE, SIG_IGN);
#endif

        // If we're the primary instance, create a server. Otherwise, create a client.
        if (mIsPrimaryInstance) {
            mSocketFd = socket(AF_INET, SOCK_STREAM, 0);
            if (mSocketFd == INVALID_SOCKET) {
                throw runtime_error{tfm::format("socket() call failed: %s", errorString(lastSocketError()))};
            }

            makeSocketNonBlocking(mSocketFd);

            // Avoid address in use error that occurs if we quit with a client connected.
            int t = 1;
            if (setsockopt(mSocketFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&t, sizeof(int)) == SOCKET_ERROR) {
                throw runtime_error{tfm::format("setsockopt() call failed: %s", errorString(lastSocketError()))};
            }

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)atoi(port.c_str()));

#ifdef _WIN32
            InetPton(AF_INET, ip.c_str(), &addr.sin_addr);
#else
            inet_aton(ip.c_str(), &addr.sin_addr);
#endif

            if (::bind(mSocketFd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                throw runtime_error{tfm::format("bind() call failed: %s", errorString(lastSocketError()))};
            }

            if (listen(mSocketFd, 5) == SOCKET_ERROR) {
                throw runtime_error{tfm::format("listen() call failed: %s", errorString(lastSocketError()))};
            }

            tlog::success() << "Initialized IPC, listening on " << ip << ":" << port;
        } else {
            struct addrinfo hints = {}, *addrinfo;
            hints.ai_family = PF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            int err = getaddrinfo(ip.c_str(), port.c_str(), &hints, &addrinfo);
            if (err != 0) {
                throw runtime_error{tfm::format("getaddrinfo() failed: %s", gai_strerror(err))};
            }

            ScopeGuard addrinfoGuard{[addrinfo] { freeaddrinfo(addrinfo); }};

            mSocketFd = INVALID_SOCKET;
            for (struct addrinfo* ptr = addrinfo; ptr; ptr = ptr->ai_next) {
                mSocketFd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
                if (mSocketFd == INVALID_SOCKET) {
                    tlog::warning() << tfm::format("socket() failed: %s", errorString(lastSocketError()));
                    continue;
                }

                if (connect(mSocketFd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
                    int errorId = lastSocketError();
                    if (errorId == SocketError::ConnRefused) {
                        throw runtime_error{"Connection to primary instance refused"};
                    } else {
                        tlog::warning() << tfm::format("connect() failed: %s", errorString(errorId));
                    }

                    closeSocket(mSocketFd);
                    mSocketFd = INVALID_SOCKET;
                    continue;
                }

                tlog::success() << "Connected to primary instance at " << ip << ":" << port;
                break; // success
            }

            if (mSocketFd == INVALID_SOCKET) {
                throw runtime_error{"Unable to connect to primary instance."};
            }
        }
    } catch (runtime_error e) {
        tlog::warning() << "Could not initialize IPC; assuming primary instance. " << e.what();
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
        unlink(mLockFile.str().c_str());
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

void Ipc::sendToPrimaryInstance(const IpcPacket& message) {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to send to the primary instance."};
    }

    int bytesSent = send(mSocketFd, message.data(), (int)message.size(), 0 /* flags */);
    if (bytesSent != int(message.size())) {
        throw runtime_error{tfm::format("send() failed: %s", errorString(lastSocketError()))};
    }
}

void Ipc::receiveFromSecondaryInstance(function<void(const IpcPacket&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    // Check for new connections.
    struct sockaddr_in client;
    socklen_t addrlen = sizeof(client);
    int fd = accept(mSocketFd, (struct sockaddr*)&client, &addrlen);
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
        tlog::info() << tfm::format("Accepted IPC client connection into socket fd %d (host: %d.%d.%d.%d:%d)", fd, ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, port);
        mSocketConnections.push_back(SocketConnection(fd));
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

Ipc::SocketConnection::SocketConnection(Ipc::socket_t fd) : mSocketFd(fd) {
    TEV_ASSERT(mSocketFd != INVALID_SOCKET, "SocketConnection must receive a valid socket.");

    makeSocketNonBlocking(mSocketFd);

    // 1MB ought to be enough for each individual packet.
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
            tlog::info() << "Client disconnected from socket fd " << mSocketFd;
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
                tlog::warning() << "Client attempted to send a packet larger than our buffer size. Connection terminated.";
                close();
                return;
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
        // but it's probably not worth the trouble.
        if (processedOffset > 0) {
            // There's a partial message; copy it to the start of 'buffer'
            // and update the offsets accordingly.
            memcpy(mBuffer.data(), mBuffer.data() + processedOffset, mRecvOffset - processedOffset);
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
