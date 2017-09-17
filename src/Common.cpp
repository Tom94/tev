// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Common.h>

#include <algorithm>
#include <cctype>
#include <map>

#ifndef _WIN32
#   include <arpa/inet.h>
#   include <cstring>
#   include <cerrno>
#   include <fcntl.h>
#   include <pwd.h>
#   include <sys/file.h>
#   include <sys/types.h>
#   include <unistd.h>
#   define SOCKET_ERROR (-1)
#   define INVALID_SOCKET (-1)
#endif

using namespace std;

TEV_NAMESPACE_BEGIN

vector<string> split(string text, const string& delim) {
    vector<string> result;
    while (true) {
        size_t begin = text.find_last_of(delim);
        if (begin == string::npos) {
            result.emplace_back(text);
            return result;
        } else {
            result.emplace_back(text.substr(begin + 1));
            text.resize(begin);
        }
    }

    return result;
}

string toLower(string str) {
    transform(begin(str), end(str), begin(str), [](unsigned char c) { return (char)tolower(c); });
    return str;
}

string toUpper(string str) {
    transform(begin(str), end(str), begin(str), [](unsigned char c) { return (char)toupper(c); });
    return str;
}

bool matches(string text, string filter) {
    if (filter.empty()) {
        return true;
    }

    // Perform matching on lowercase strings
    text = toLower(text);
    filter = toLower(filter);

    auto words = split(filter, ", ");
    // We don't want people entering multiple spaces in a row to match everything.
    words.erase(remove(begin(words), end(words), ""), end(words));

    if (words.empty()) {
        return true;
    }

    // Match every word of the filter separately.
    for (const auto& word : words) {
        if (text.find(word) != string::npos) {
            return true;
        }
    }

    return false;
}

ETonemap toTonemap(string name) {
    // Perform matching on uppercase strings
    name = toUpper(name);
    if (name == "SRGB") {
        return SRGB;
    } else if (name == "GAMMA") {
        return Gamma;
    } else if (name == "FALSECOLOR" || name == "FC") {
        return FalseColor;
    } else if (name == "POSITIVENEGATIVE" || name == "POSNEG" || name == "PN" ||name == "+-") {
        return PositiveNegative;
    } else {
        return SRGB;
    }
}

EMetric toMetric(string name) {
    // Perform matching on uppercase strings
    name = toUpper(name);
    if (name == "E") {
        return Error;
    } else if (name == "AE") {
        return AbsoluteError;
    } else if (name == "SE") {
        return SquaredError;
    } else if (name == "RAE") {
        return RelativeAbsoluteError;
    } else if (name == "RSE") {
        return RelativeSquaredError;
    } else {
        return Error;
    }
}

static string errorString(int errorId) {
#ifdef _WIN32
    char* s = NULL;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorId, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&s, 0, NULL);

    string result = tfm::format("%s (%d)", s, errorId);
    LocalFree(s);

    return result;
#else
    return tfm::format("%s (%d)", strerror(errorId), errno);
#endif
}

static int lastError() {
#ifdef _WIN32
    return GetLastError();
#else
    return errno;
#endif
}

static int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

string absolutePath(string path) {
    const static int bufferSize = 16384;
    char buffer[bufferSize];

#ifdef _WIN32
    DWORD length = GetFullPathName(path.c_str(), bufferSize, buffer, NULL);
    if (length == 0 || length == bufferSize) {
        throw runtime_error{tfm::format("Could not obtain absolute path: %s", errorString(lastError()))};
    }
    return buffer;
#else
    if (realpath(path.c_str(), buffer) == NULL) {
        throw runtime_error{tfm::format("Could not obtain absolute path: %s", errorString(lastError()))};
    }
    return buffer;
#endif
}

void toggleConsole() {
#ifdef _WIN32
    HWND console = GetConsoleWindow();
    DWORD consoleProcessId;
    GetWindowThreadProcessId(console, &consoleProcessId);

    // Only toggle the console if it was actually spawned by tev. If we are
    // running in a foreign console, then we should leave it be.
    if (GetCurrentProcessId() == consoleProcessId) {
        ShowWindow(console, IsWindowVisible(console) ? SW_HIDE : SW_SHOW);
    }
#endif
}

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
        struct passwd* pw = getpwuid(getuid());
        string home = pw->pw_dir;
        mLockFile = home + "/" + lockName;

        mLockFileDescriptor = open(mLockFile.c_str(), O_RDWR | O_CREAT, 0666);
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
        cerr << "Error initializing IPC. " << e.what() << endl;
        mIsPrimaryInstance = true;
    }
}

void Ipc::sendToPrimaryInstance(string message) {
    int bytesSent = sendto(mSocket, message.c_str(), (int)message.length() + 1, 0, (sockaddr*)&mAddress, sizeof(mAddress));
    if (bytesSent == -1) {
        throw runtime_error{tfm::format("Could not send to primary instance: %s", errorString(lastSocketError()))};
    }
}

bool Ipc::receiveFromSecondaryInstance(function<void(string)> callback) {
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
        unlink(mLockFile.c_str());
    }

    if (mSocket != INVALID_SOCKET) {
        close(mSocket);
    }
#endif
}

TEV_NAMESPACE_END
