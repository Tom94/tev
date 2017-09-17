// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#ifndef _WIN32
#   include <netinet/in.h>
#endif

TEV_NAMESPACE_BEGIN

class Ipc {
public:
    Ipc();
    virtual ~Ipc();

    bool isPrimaryInstance() {
        return mIsPrimaryInstance;
    }

    void sendToPrimaryInstance(const std::string& message);
    bool receiveFromSecondaryInstance(std::function<void(const std::string&)> callback);

private:
    bool mIsPrimaryInstance;
    sockaddr_in mAddress;

#ifdef _WIN32
    HANDLE mInstanceMutex;
    SOCKET mSocket;
#else
    int mLockFileDescriptor;
    int mSocket;
    std::string mLockFile;
#endif
};

TEV_NAMESPACE_END
