// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Ipc.h>

#ifdef _WIN32
using socklen_t = int;
#else
#   include <arpa/inet.h>
#   include <cstring>
#   include <sys/file.h>
#   include <unistd.h>
#   define SOCKET_ERROR (-1)
#   define INVALID_SOCKET (-1)
#endif

using namespace std;

TEV_NAMESPACE_BEGIN

Ipc::Ipc() {
    static const string lockName = ".tev-lock";
    static const string localhost = "127.0.0.1";
    static const short port = 14158;

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
        mAddress.sin_addr.S_un.S_addr = inet_addr(localhost.c_str());
#else
        inet_aton(localhost.c_str(), &mAddress.sin_addr);
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
        tlog::Warning() << "Could not initialize IPC; assuming primary instance. " << e.what();
        mIsPrimaryInstance = true;
    }
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

void Ipc::sendToPrimaryInstance(const string& message) {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to send to the primary instance."};
    }

    int bytesSent = sendto(mSocket, message.c_str(), (int)message.length() + 1, 0, (sockaddr*)&mAddress, sizeof(mAddress));
    if (bytesSent == -1) {
        throw runtime_error{tfm::format("Could not send to primary instance: %s", errorString(lastSocketError()))};
    }
}

bool Ipc::receiveFromSecondaryInstance(function<void(const string&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    sockaddr_in recvAddress;
    socklen_t recvAddressSize = sizeof(recvAddress);
    char recvBuffer[16384];

    int bytesReceived = recvfrom(mSocket, recvBuffer, sizeof(recvBuffer), 0, (sockaddr*)&recvAddress, &recvAddressSize);
    if (bytesReceived <= 0) {
        return false;
    }

    if (recvBuffer[bytesReceived - 1] == '\0') {
        // We probably received a valid string. Let's pass it to our callback function.
        // TODO: Proper handling of multiple concatenated and partial packets.
        callback(recvBuffer);
    }

    return true;
}

TEV_NAMESPACE_END
