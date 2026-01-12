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

#ifdef _WIN32
#    define NOMINMAX
#    include <Ws2tcpip.h>
#    include <afunix.h>
#    include <winsock2.h>
#    undef NOMINMAX
#endif

#include <tev/Common.h>
#include <tev/Ipc.h>
#include <tev/ThreadPool.h>

#ifdef _WIN32
using socklen_t = int;
#else
#    include <arpa/inet.h>
#    include <cstring>
#    ifdef EMSCRIPTEN
#        include <fcntl.h>
#    endif
#    include <netdb.h>
#    include <netinet/in.h>
#    include <signal.h>
#    include <sys/file.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

#include <filesystem>
#include <variant>

using namespace std;

namespace tev {

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

    mPayload.assign(data, data + length);
}

void IpcPacket::setOpenImage(string_view imagePath, string_view channelSelector, bool grabFocus) {
    OStream payload{mPayload};
    payload << EType::OpenImageV2;
    payload << grabFocus;
    payload << imagePath;
    payload << channelSelector;
}

void IpcPacket::setReloadImage(string_view imageName, bool grabFocus) {
    OStream payload{mPayload};
    payload << EType::ReloadImage;
    payload << grabFocus;
    payload << imageName;
}

void IpcPacket::setCloseImage(string_view imageName) {
    OStream payload{mPayload};
    payload << EType::CloseImage;
    payload << imageName;
}

void IpcPacket::setUpdateImage(
    string_view imageName,
    bool grabFocus,
    span<const IpcPacket::ChannelDesc> channelDescs,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    span<const float> stridedImageData
) {
    if (channelDescs.empty()) {
        throw runtime_error{"UpdateImage IPC packet must have a non-zero channel count."};
    }

    const int32_t nChannels = (int32_t)channelDescs.size();
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

    const size_t nPixels = width * height;

    size_t stridedImageDataSize = 0;
    for (int32_t c = 0; c < nChannels; ++c) {
        stridedImageDataSize = max(stridedImageDataSize, (size_t)(channelOffsets[c] + (nPixels - 1) * channelStrides[c] + 1));
    }

    if (stridedImageData.size() != stridedImageDataSize) {
        throw runtime_error{fmt::format(
            "UpdateImage IPC packet's data size does not match specified dimensions, offset, and stride. (Expected: {})", stridedImageDataSize
        )};
    }

    payload << stridedImageData;
}

void IpcPacket::setCreateImage(
    string_view imageName, bool grabFocus, int32_t width, int32_t height, int32_t nChannels, span<const string> channelNames
) {
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

void IpcPacket::setVectorGraphics(string_view imageName, bool grabFocus, bool append, span<const VgCommand> commands) {
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

    const size_t colonPos = imageString.find_last_of(":");
    if (colonPos == string::npos ||
        // windows path of the form X:/* or X:\*
        (colonPos == 1 && imageString.length() >= 3 && (imageString[2] == '\\' || imageString[2] == '/'))) {
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
    const size_t nPixels = (size_t)result.width * result.height;

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
        stridedImageDataSize = max(stridedImageDataSize, (size_t)(result.channelOffsets[c] + (nPixels - 1) * result.channelStrides[c] + 1));
    }

    if (payload.remainingBytes() < stridedImageDataSize * sizeof(float)) {
        throw runtime_error{"UpdateImage: insufficient image data."};
    }

    const float* stridedImageData = (const float*)payload.get();
    ThreadPool::global()
        .parallelForAsync<size_t>(
            0,
            nPixels,
            nPixels * result.nChannels,
            [&](size_t px) {
                for (int32_t c = 0; c < result.nChannels; ++c) {
                    result.imageData[c][px] = stridedImageData[result.channelOffsets[c] + px * result.channelStrides[c]];
                }
            },
            numeric_limits<int>::max()
        )
        .get();

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
        throw runtime_error{fmt::format("ioctlsocket() to make socket non-blocking failed: {}", errorString(ioctlsocketResult))};
    }
#else
    if (fcntl(socketFd, F_SETFL, fcntl(socketFd, F_GETFL, 0) | O_NONBLOCK) == SOCKET_ERROR) {
        throw runtime_error{fmt::format("fcntl() to make socket non-blocking failed: {}", errorString(lastSocketError()))};
    }
#endif
}

static int shutdownSocketWrite(Ipc::socket_t socket) {
#ifdef _WIN32
    return shutdown(socket, SD_SEND);
#else
    return shutdown(socket, SHUT_WR);
#endif
}

static int closeSocket(Ipc::socket_t socket) {
#ifdef _WIN32
    return closesocket(socket);
#else
    return close(socket);
#endif
}

Ipc::Ipc(string_view hostname) : mSocketFd{INVALID_SOCKET} {
    if (hostname.empty()) {
        if (!flatpakInfo() || flatpakInfo()->hasNetworkAccess()) {
            hostname = "127.0.0.1:14158";
        } else {
            hostname = "unix";
        }
    }

    try {
        fs::create_directories(runtimeDirectory());
    } catch (const fs::filesystem_error& e) {
        tlog::warning() << fmt::format("Runtime directory {} does not exist and could not be created: {}", runtimeDirectory(), e.what());
    }

    mLockName = fmt::format(".tev.{}.lock", hostname);

    const auto parts = split(hostname, ":");
    if (parts.size() == 1 || (parts.size() == 2 && parts.back() == "unix")) {
        mHostInfo = UnixHost{.socketPath = runtimeDirectory() / fmt::format(".tev.{}.sock", parts.front())};
        tlog::debug() << fmt::format("Initializing IPC on unix socket {}", this->hostname());
    } else if (parts.size() == 2) {
        mHostInfo = IpHost{.ip = string{parts.front()}, .port = string{parts.back()}};
        tlog::debug() << fmt::format("Initializing IPC on IP host {}", this->hostname());
    } else {
        throw runtime_error{fmt::format("IPC hostname must not include more than one ':' symbol but is {}.", hostname)};
    }

    try {
        // Networking
#ifdef _WIN32
        // FIXME: only do this once if multiple Ipc objects are created.
        WSADATA wsaData;
        int wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaStartupResult != NO_ERROR) {
            throw runtime_error{fmt::format("Could not initialize WSA: {}", errorString(wsaStartupResult))};
        }
#else
        // We don't care about getting a SIGPIPE if the display server goes away...
        signal(SIGPIPE, SIG_IGN);
#endif

        if (attemptToBecomePrimaryInstance()) {
            return;
        }

        // If we're not the primary instance, try to connect to it as a client

        struct addrinfo addrinfo = {}, *heapaddrinfo = nullptr;
        struct sockaddr_un unixAddr = {};

        visit(
            [&](auto&& hostInfo) {
                using T = decay_t<decltype(hostInfo)>;
                if constexpr (is_same_v<T, IpHost>) {
                    addrinfo.ai_family = PF_UNSPEC;
                    addrinfo.ai_socktype = SOCK_STREAM;

                    const int err = getaddrinfo(hostInfo.ip.c_str(), hostInfo.port.c_str(), &addrinfo, &heapaddrinfo);
                    if (err != 0) {
                        throw runtime_error{fmt::format("getaddrinfo() failed: {}", gai_strerror(err))};
                    }

                    addrinfo = *heapaddrinfo;
                } else if constexpr (is_same_v<T, UnixHost>) {
                    // Note: if the socket file does not exist, connect() will fail, so we do not need to separately check for its existence
                    // here. Furthermore, on Windows, checking for the existence of a unix socket file raises an error.

                    addrinfo.ai_family = AF_UNIX;
                    addrinfo.ai_socktype = SOCK_STREAM;
                    addrinfo.ai_addrlen = sizeof(struct sockaddr_un);
                    addrinfo.ai_addr = (struct sockaddr*)&unixAddr;

                    unixAddr.sun_family = AF_UNIX;
                    strncpy(unixAddr.sun_path, hostInfo.socketPath.string().c_str(), sizeof(unixAddr.sun_path) - 1);
                } else {
                    TEV_ASSERT(false, "Non-exhaustive visitor");
                }
            },
            mHostInfo
        );

        ScopeGuard addrinfoGuard{[heapaddrinfo] { freeaddrinfo(heapaddrinfo); }};

        mSocketFd = INVALID_SOCKET;
        for (struct addrinfo* ptr = &addrinfo; ptr; ptr = ptr->ai_next) {
            mSocketFd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (mSocketFd == INVALID_SOCKET) {
                tlog::warning() << fmt::format("socket() failed: {}", errorString(lastSocketError()));
                continue;
            }

            if (connect(mSocketFd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
                int errorId = lastSocketError();
                if (errorId == SocketError::ConnRefused) {
                    throw runtime_error{"Connection to primary instance refused"};
                } else {
                    tlog::warning() << fmt::format("connect() failed: {}", errorString(errorId));
                }

                closeSocket(mSocketFd); // discard socket closure error
                mSocketFd = INVALID_SOCKET;
                continue;
            }

            tlog::success() << fmt::format("Connected to primary instance {}", this->hostname());
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
    tlog::debug() << "Shutting down IPC.";

    if (mSocketFd != INVALID_SOCKET) {
        if (!mIsPrimaryInstance) {
            sendRemainingDataAndDisconnectFromPrimaryInstance();
        }

        if (closeSocket(mSocketFd) == SOCKET_ERROR) {
            tlog::warning() << fmt::format("Error closing socket {}: {}", mSocketFd, errorString(lastSocketError()));
        }
    }

    if (holds_alternative<UnixHost>(mHostInfo)) {
        // Delete the unix socket file if it exists.
        fs::remove(get<UnixHost>(mHostInfo).socketPath);
    }

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
        fs::remove(mLockFile);
    }
#endif

#ifdef _WIN32
    // FIXME: only do this when the last Ipc is destroyed
    WSACleanup();
#endif
}

void Ipc::sendRemainingDataAndDisconnectFromPrimaryInstance() {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to disconnect from the primary instance."};
    }

    if (mSocketFd == INVALID_SOCKET) {
        return;
    }

    const bool successfulShutdown = shutdownSocketWrite(mSocketFd) != SOCKET_ERROR;
    if (!successfulShutdown) {
        tlog::warning() << fmt::format("Error shutting down socket {}: {}", mSocketFd, errorString(lastSocketError()));
    } else {
        const auto start = chrono::steady_clock::now();
        const auto timeout = chrono::seconds{5};

        // Drain any remaining incoming data. Only when done successfully we can be sure that the peer received all the data we sent and
        // subsequently received our shutdown.
        char buffer[4096];
        while (chrono::steady_clock::now() - start < timeout) {
            const int nReceived = recv(mSocketFd, buffer, sizeof(buffer), 0);
            if (nReceived == SOCKET_ERROR) {
                const int errorId = lastSocketError();
                if (errorId == SocketError::Again || errorId == SocketError::WouldBlock) {
                    this_thread::sleep_for(1ms);
                    continue;
                } else {
                    tlog::warning()
                        << fmt::format("Error receiving final data from primary instance ({}: {})", mSocketFd, errorString(errorId));
                    break;
                }
            } else if (nReceived == 0) {
                break;
            }
        }

        if (chrono::steady_clock::now() - start >= timeout) {
            tlog::warning()
                << fmt::format("Timeout of {} seconds while disconnecting from primary instance {}", timeout.count(), this->hostname());
        }

        tlog::debug() << fmt::format("Gracefully disconnected from primary instance {}", this->hostname());
    }
}

bool Ipc::isConnectedToPrimaryInstance() const { return !mIsPrimaryInstance && mSocketFd != INVALID_SOCKET; }

bool Ipc::attemptToBecomePrimaryInstance() {
#ifdef _WIN32
    // Make sure at most one instance of tev is running
    mInstanceMutex = CreateMutex(NULL, TRUE, mLockName.c_str());

    if (!mInstanceMutex) {
        throw runtime_error{fmt::format("Could not obtain global mutex: {}", errorString(lastError()))};
    }

    mIsPrimaryInstance = GetLastError() != ERROR_ALREADY_EXISTS;
    if (!mIsPrimaryInstance) {
        // No need to keep the handle to the existing mutex if we're not the primary instance.
        ReleaseMutex(mInstanceMutex);
        CloseHandle(mInstanceMutex);
    }
#else
    mLockFile = runtimeDirectory() / mLockName;

    mLockFileDescriptor = open(mLockFile.string().c_str(), O_RDWR | O_CREAT, 0666);
    if (mLockFileDescriptor == -1) {
        throw runtime_error{fmt::format("Could not create lock file: {}", errorString(lastError()))};
    }

    tlog::debug() << fmt::format("Lock file {} created or exists", mLockFile);

    mIsPrimaryInstance = !flock(mLockFileDescriptor, LOCK_EX | LOCK_NB);
    if (!mIsPrimaryInstance) {
        tlog::debug() << fmt::format("Could not acquire lock. Must be secondary instance.");
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

    // Set up primary instance listening socket
    mSocketFd = socket(holds_alternative<IpHost>(mHostInfo) ? AF_INET : AF_UNIX, SOCK_STREAM, 0);
    if (mSocketFd == INVALID_SOCKET) {
        throw runtime_error{fmt::format("socket() call failed: {}", errorString(lastSocketError()))};
    }

    makeSocketNonBlocking(mSocketFd);

    if (holds_alternative<IpHost>(mHostInfo)) {
        // Avoid address in use error that occurs if we quit with a client connected.
        int t = 1;
        if (setsockopt(mSocketFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&t, sizeof(int)) == SOCKET_ERROR) {
            throw runtime_error{fmt::format("setsockopt() call failed: {}", errorString(lastSocketError()))};
        }
    }

    union {
        struct sockaddr_in in;
        struct sockaddr_un un;
    } addr;
    const size_t addrLen = holds_alternative<IpHost>(mHostInfo) ? sizeof(addr.in) : sizeof(addr.un);

    visit(
        [&](auto&& hostInfo) {
            using T = decay_t<decltype(hostInfo)>;
            if constexpr (is_same_v<T, IpHost>) {
                addr.in.sin_family = AF_INET;
                addr.in.sin_port = htons((uint16_t)atoi(hostInfo.port.c_str()));
#ifdef _WIN32
                InetPton(AF_INET, hostInfo.ip.c_str(), &addr.in.sin_addr);
#else
                inet_aton(hostInfo.ip.c_str(), &addr.in.sin_addr);
#endif
            } else if constexpr (is_same_v<T, UnixHost>) {
                addr.un.sun_family = AF_UNIX;
                strncpy(addr.un.sun_path, hostInfo.socketPath.string().c_str(), sizeof(addr.un.sun_path) - 1);
                fs::remove(hostInfo.socketPath); // remove previous socket file if it exists
            } else {
                TEV_ASSERT(false, "Non-exhaustive visitor");
            }
        },
        mHostInfo
    );

    if (::bind(mSocketFd, (const struct sockaddr*)&addr, addrLen) == SOCKET_ERROR) {
        throw runtime_error{fmt::format("bind() call failed: {}", errorString(lastSocketError()))};
    }

    if (listen(mSocketFd, 5) == SOCKET_ERROR) {
        throw runtime_error{fmt::format("listen() call failed: {}", errorString(lastSocketError()))};
    }

    tlog::success() << fmt::format("Initialized IPC, listening on {}", this->hostname());
    return true;
}

void Ipc::sendToPrimaryInstance(const IpcPacket& message) {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to send to the primary instance."};
    }

    const int bytesSent = (int)send(mSocketFd, message.data(), (int)message.size(), 0 /* flags */);
    if (bytesSent != int(message.size())) {
        throw runtime_error{fmt::format("send() failed: {}", errorString(lastSocketError()))};
    }

    mNTotalBytesSent += message.size();
}

void Ipc::receiveFromSecondaryInstance(function<void(const IpcPacket&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    if (mSocketFd == INVALID_SOCKET) {
        return;
    }

    // Check for new connections.
    union {
        struct sockaddr_in in;
        struct sockaddr_un un;
    } client;
    socklen_t addrlen = sizeof(client);

    const socket_t fd = accept(mSocketFd, (struct sockaddr*)&client, &addrlen);
    if (fd == INVALID_SOCKET) {
        const int errorId = lastSocketError();
        if (errorId != SocketError::Again && errorId != SocketError::WouldBlock) {
            tlog::warning() << "accept() error: " << errorId << " " << errorString(errorId);
        }
    } else {
        string name = "";
        visit(
            [&](auto&& hostInfo) {
                using T = decay_t<decltype(hostInfo)>;
                if constexpr (is_same_v<T, UnixHost>) {
                    name = fmt::format("{}", hostInfo.socketPath);
                } else if constexpr (is_same_v<T, IpHost>) {
                    const uint32_t ip = ntohl(client.in.sin_addr.s_addr);
                    const uint16_t port = ntohs(client.in.sin_port);
                    name = fmt::format("{}.{}.{}.{}:{}", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, port);
                } else {
                    TEV_ASSERT(false, "Non-exhaustive visitor");
                }
            },
            mHostInfo
        );

        tlog::info() << fmt::format("Client {} (#{}) connected", name, fd);
        mSocketConnections.push_back(SocketConnection{fd, name});
    }

    // Service existing connections.
    for (auto iter = mSocketConnections.begin(); iter != mSocketConnections.end();) {
        const auto cur = iter++;
        mNTotalBytesReceived += cur->service(callback);

        // If the connection became closed, stop keeping track of it.
        if (cur->isClosed()) {
            mSocketConnections.erase(cur);
        }
    }
}

string Ipc::hostname() const {
    string result = "";
    visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, IpHost>) {
                result = fmt::format("{}:{}", arg.ip, arg.port);
            } else if constexpr (std::is_same_v<T, UnixHost>) {
                result = fmt::format("{}", arg.socketPath);
            } else {
                TEV_ASSERT(false, "Non-exhaustive visitor");
            }
        },
        mHostInfo
    );

    return result;
}

Ipc::SocketConnection::SocketConnection(Ipc::socket_t fd, string_view name) : mSocketFd{fd}, mName{name} {
    TEV_ASSERT(mSocketFd != INVALID_SOCKET, "SocketConnection must receive a valid socket.");

    makeSocketNonBlocking(mSocketFd);

    // 1 MiB is a good default buffer size. If larger is required, it'll be automatizally resized.
    mBuffer.resize(1024 * 1024);
}

Ipc::SocketConnection::~SocketConnection() { close(); }

size_t Ipc::SocketConnection::service(function<void(const IpcPacket&)> callback) {
    if (isClosed()) {
        return 0;
    }

    size_t nTotalBytesReceived = 0;
    while (true) {
        // Receive as much data as we can, up to the capacity of 'mBuffer'.
        const size_t maxBytes = mBuffer.size() - mRecvOffset;
        const int bytesReceived = (int)recv(mSocketFd, mBuffer.data() + mRecvOffset, (int)maxBytes, 0);
        if (bytesReceived == SOCKET_ERROR) {
            const int errorId = lastSocketError();
            if (errorId != SocketError::Again && errorId != SocketError::WouldBlock) {
                tlog::warning() << "Error while reading from socket. " << errorString(errorId) << " Connection terminated.";
                close();
                break;
            }

            break; // try again later
        } else if (bytesReceived == 0) {
            tlog::info() << "Client " << mName << " (#" << mSocketFd << ") disconnected";
            close();
            break;
        }

        TEV_ASSERT(bytesReceived > 0, "With no error, the number of bytes received should be positive.");
        mRecvOffset += (size_t)bytesReceived;
        nTotalBytesReceived += (size_t)bytesReceived;

        // Go through the buffer and service as many complete messages as we can find.
        size_t processedOffset = 0;
        while (processedOffset + 4 <= mRecvOffset) {
            // There's at least enough to figure out the next message's length.
            const char* const messagePtr = mBuffer.data() + processedOffset;
            const uint32_t messageLength = *((uint32_t*)messagePtr);

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

        // TODO: we could save the memcpy by treating 'buffer' as a ring-buffer, but it's probably not worth the trouble. Revisit when
        // someone throws around buffers with a size of gigabytes.
        if (processedOffset > 0) {
            // There's a partial message; copy it to the start of 'buffer' and update the offsets accordingly.
            memmove(mBuffer.data(), mBuffer.data() + processedOffset, mRecvOffset - processedOffset);
            mRecvOffset -= processedOffset;
        }
    }

    return nTotalBytesReceived;
}

void Ipc::SocketConnection::close() {
    if (!isClosed()) {
        shutdownSocketWrite(mSocketFd);
        closeSocket(mSocketFd);
        mSocketFd = INVALID_SOCKET;
    }
}

bool Ipc::SocketConnection::isClosed() const { return mSocketFd == INVALID_SOCKET; }

} // namespace tev
