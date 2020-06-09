// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Common.h>
#include <tev/Ipc.h>

#ifndef _WIN32
#   include <cstring>
#   ifdef EMSCRIPTEN
#       include <fcntl.h>
#   endif
#   include <signal.h>
#   include <sys/file.h>
#   include <sys/socket.h>
#   include <unistd.h>
#   include <errno.h>
#   include <arpa/inet.h>
#   include <netdb.h>
#   include <netinet/in.h>
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
    payload << int(0); // save space for message length
    payload << Type::OpenImage;
    payload << grabFocus;
    payload << imagePath;
    setMessageLength();
}

void IpcPacket::setReloadImage(const string& imageName, bool grabFocus) {
    OStream payload{mPayload};
    payload << int(0); // save space for message length
    payload << Type::ReloadImage;
    payload << grabFocus;
    payload << imageName;
    setMessageLength();
}

void IpcPacket::setCloseImage(const string& imageName) {
    OStream payload{mPayload};
    payload << Type::CloseImage;
    payload << imageName;
    setMessageLength();
}

void IpcPacket::setUpdateImage(const string& imageName, bool grabFocus, const string& channel, int x, int y, int width, int height, const vector<float>& imageData) {
    if ((int)imageData.size() != width * height) {
        throw runtime_error{"UpdateImage IPC packet's data size does not match crop windows."};
    }

    OStream payload{mPayload};
    payload << int(0); // save space for message length
    payload << Type::UpdateImage;
    payload << grabFocus;
    payload << imageName;
    payload << channel;
    payload << x << y << width << height;
    payload << imageData;
    setMessageLength();
}

void IpcPacket::setCreateImage(const std::string& imageName, bool grabFocus, int width, int height, int nChannels, const std::vector<std::string>& channelNames) {
    if ((int)channelNames.size() != nChannels) {
        throw runtime_error{"CreateImage IPC packet's channel names size does not match number of channels."};
    }

    OStream payload{mPayload};
    payload << int(0); // save space for message length
    payload << Type::CreateImage;
    payload << grabFocus;
    payload << imageName;
    payload << width << height;
    payload << nChannels;
    payload << channelNames;
    setMessageLength();
}

IpcPacketOpenImage IpcPacket::interpretAsOpenImage() const {
    IpcPacketOpenImage result;
    IStream payload{mPayload};

    int messageLength;
    payload >> messageLength;  // unused

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

    int messageLength;
    payload >> messageLength;  // unused

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

    int messageLength;
    payload >> messageLength;  // unused

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

    int messageLength;
    payload >> messageLength;  // unused

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

    int messageLength;
    payload >> messageLength;  // unused

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
            throw runtime_error{tfm::format("Could not create lock file: ", errorString(lastError()))};
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
        if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
            throw runtime_error{"Could not initialize WinSock"};
        }
#else
        // We don't care about getting a SIGPIPE if the display server goes
        // away...
        signal(SIGPIPE, SIG_IGN);
#endif

        // If we're the primary instance, create a server. Otherwise, create a client.
        if (mIsPrimaryInstance) {
            socketFd = socket(AF_INET, SOCK_STREAM, 0);
            if (socketFd == -1) {
                throw runtime_error{"socket() call failed"};
            }

            if (fcntl(socketFd, F_SETFL,
                      fcntl(socketFd, F_GETFL, 0) | O_NONBLOCK) == -1) {
                throw runtime_error{"fcntl() to make socket non-blocking failed"};
            }

            // Avoid address in use error that occurs if we quit with a
            // client connected.
            int t = 1;
            int ret = setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, (const char *)&t,
                                 sizeof(int));
            if (ret == -1) {
                throw runtime_error{"setsockopt() call failed"};
            }

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons((short)atoi(port.c_str()));

            if (::bind(socketFd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
                throw runtime_error{"bind() call failed"};
            }

            if (listen(socketFd, 5) == -1) {
                throw runtime_error{"listen() call failed"};
            }

            tlog::success() << "Initialized IPC, listening on " << ip << ":" << port;
        } else {
            struct addrinfo hints = {}, *addrinfo;
            hints.ai_family = PF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            int err = getaddrinfo(ip.c_str(), port.c_str(), &hints, &addrinfo);
            if (err != 0) {
                throw runtime_error{tfm::format("getaddrinfo: %s", gai_strerror(err))};
            }

            socketFd = -1;
            for (struct addrinfo* ptr = addrinfo; ptr; ptr = ptr->ai_next) {
                socketFd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
                if (socketFd < 0) {
                    tlog::info() << tfm::format("socket() failed: %s", errorString(lastError()));
                    continue;
                }

                if (connect(socketFd, ptr->ai_addr, ptr->ai_addrlen) < 0) {
                    if (errno == ECONNREFUSED) {
                        throw runtime_error{"Connection to primary refused"};
                    }
                    else {
                        tlog::info() << tfm::format("connect() failed: %s", errorString(lastError()));
                    }

                    close(socketFd);
                    socketFd = -1;
                    continue;
                }

                tlog::info() << "Connected to primary instance";
                break; // success
            }

            freeaddrinfo(addrinfo);
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
    if (socketFd != -1) {
        if (close(socketFd) == -1) {
            tlog::warning() << "Error closing socket listen fd " << socketFd;
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

    int bytesSent = send(socketFd, message.data(), message.size(), 0 /* flags */);
    if (bytesSent != int(message.size())) {
        throw runtime_error{tfm::format("send() failed: %s", errorString(lastError()))};
    }
}

void Ipc::receiveFromSecondaryInstance(function<void(const IpcPacket&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    // Check for new connections.
    struct sockaddr_in client;
    socklen_t addrlen = sizeof(client);
    int fd = accept(socketFd, (struct sockaddr *)&client, &addrlen);
    if (fd == -1) {
        if (errno == EWOULDBLOCK) {
            // no problem; no one is trying to connect
        } else
            tlog::error() << "accept() error: " << strerror(errno);
    } else {
        uint32_t ip = ntohl(client.sin_addr.s_addr);
        tlog::info() << tfm::format("Got socket connection from %d.%d.%d.%d", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
        // Only allow local connections (??)
        // if (client.sin_addr.s_addr != htonl(LOCALHOST_IP))
        //     close(fd);
        // else ....
        socketConnections.push_back(SocketConnection(fd));
    }

    // Service existing connections.
    for (auto iter = socketConnections.begin(); iter != socketConnections.end(); ) {
        auto cur = iter++;
        if (cur->Closed())
            socketConnections.erase(cur);
        else
            cur->Service(callback);
    }
}

SocketConnection::SocketConnection(int fd)
    : fd(fd) {
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == -1) {
        throw runtime_error{tfm::format("Could not make socket non-blocking: ", errorString(lastError()))};
    }

    buffer.resize(512 * 1024);
}

void SocketConnection::Service(function<void(const IpcPacket&)> callback) {
    if (fd == -1)
        // Client disconnected, so don't bother.
        return;

    while (true) {
        // Receive as much data as we can, up to the capacity of 'buffer'.
        int maxBytes = buffer.size() - recvOffset;
        int bytesReceived = recv(fd, buffer.data() + recvOffset, maxBytes, 0);
        if (bytesReceived == -1) {
            if (errno == EAGAIN) // no more data; this is fine.
                break;
            else
                throw runtime_error{tfm::format("Error reading from socket: ", errorString(lastError()))};
        }
        recvOffset += bytesReceived;

        // Since we aren't getting annoying SIGPIPE signals when a client
        // disconnects, a zero-byte read here is how we know when that
        // happens.
        if (bytesReceived == 0) {
            tlog::info() << "Client disconnected from socket fd " << fd;
            close(fd);
            fd = -1;
            return;
        }

        // Go through the buffer and service as many complete messages as
        // we can find.
        int processedOffset = 0;
        while (processedOffset + 4 <= recvOffset) {
            // There's at least enough to figure out the next message's
            // length.
            char* messagePtr = buffer.data() + processedOffset;
            int messageLength = *((int *)messagePtr);

            if (processedOffset + messageLength <= recvOffset) {
                // We have a full message.
                callback(IpcPacket(messagePtr, messageLength));
                processedOffset += messageLength;
            } else
                // It's a partial message; we'll need to recv() more.
                break;
        }

        // TODO: we could save the memcpy by treating 'buffer' as a ring-buffer,
        // but it's probably not worth the trouble.
        if (processedOffset > 0) {
            // There's a partial message; copy it to the start of 'buffer'
            // and update the offsets accordingly.
            memcpy(buffer.data(), buffer.data() + processedOffset, recvOffset - processedOffset);
            recvOffset -= processedOffset;
        }
    }
}

TEV_NAMESPACE_END
