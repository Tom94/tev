// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

// ENet must come first to prevent compilation failure on windows
#define NOMINMAX
#include <enet/enet.h>
#undef NOMINMAX

#include <tev/Common.h>
#include <tev/Ipc.h>

#ifndef _WIN32
#   include <cstring>
#   ifdef EMSCRIPTEN
#       include <fcntl.h>
#   endif
#   include <sys/file.h>
#   include <unistd.h>
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

    // This is a very conservative upper bound on how many floats
    // fit into a UDP packet
    if (nPixels > 100000) {
        throw runtime_error{"Too many pixels in UpdateImage IPC packet."};
    }

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

    // This is a very conservative upper bound on how many channel names
    // fit into a UDP packet
    if (result.nChannels > 10000) {
        throw runtime_error{"Too many channels in CreateImage IPC packet."};
    }

    result.channelNames.resize(result.nChannels);
    payload >> result.channelNames;

    return result;
}


Ipc::Ipc(const string& hostname) {
    const string lockName = string{".tev-lock."} + hostname;

    auto parts = split(hostname, ":");
    const string& ip = parts.front();
    short port = (short)atoi(parts.back().c_str());

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
        if (enet_initialize() != 0) {
            throw runtime_error{"Could not initialize enet"};
        }

        enet_address_set_host(&mAddress, ip.c_str());
        mAddress.port = port;

        // If we're the primary instance, create an enet server. Otherwise, create a client
        if (mIsPrimaryInstance) {
            mSocket = enet_host_create(
                &mAddress, 
                32 /* allow up to 32 clients and/or outgoing connections */,
                1  /* allow up to 1 channels to be used: 0 */,
                0  /* assume any amount of incoming bandwidth */,
                0  /* assume any amount of outgoing bandwidth */
            );

            if (!mSocket) {
                throw runtime_error{"Could not create enet server"};
            }

            tlog::success() << "Initialized IPC, listening on " << ip << ":" << port;
        } else {
            mSocket = enet_host_create(
                NULL /* create a client host */,
                1 /* only allow 1 outgoing connection */,
                1 /* allow up 1 channels to be used: 0 */,
                0 /* assume any amount of incoming bandwidth */,
                0 /* assume any amount of outgoing bandwidth */
            );

            if (!mSocket) {
                throw runtime_error{"Could not create enet client"};
            }

            mPeer = enet_host_connect(mSocket, &mAddress, 1, 0);
            if (!mPeer) {
                throw runtime_error{"Failed to create an enet peer"};
            }

            ENetEvent event;
            if (enet_host_service(mSocket, &event, 1000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
                tlog::success() << "Connected to primary instance via IPC @ " << ip << ":" << port;
            } else {
                enet_peer_reset(mPeer);
                throw runtime_error{"Failed to connect to primary instance"};
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
    if (mPeer) {
        ENetEvent event;
        
        // Allow 500ms to send remaining commands in the pipeline
        while (enet_host_service(mSocket, &event, 500) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_RECEIVE:
                    enet_packet_destroy(event.packet);
                    break;
                default: break;
            }
        }

        enet_peer_disconnect(mPeer, 0);

        // Allow up to 500ms for the peer to disconnect
        while (enet_host_service(mSocket, &event, 1000) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_RECEIVE:
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    tlog::success() << "Disconnected gracefully from primary instance";
                    return;
                default: break;
            }
        }

        enet_peer_reset(mPeer);
    }

    if (mSocket) {
        enet_host_destroy(mSocket);
    }

    enet_deinitialize();
}

void Ipc::sendToPrimaryInstance(const IpcPacket& message) {
    if (mIsPrimaryInstance) {
        throw runtime_error{"Must be a secondary instance to send to the primary instance."};
    }

    ENetPacket* packet = enet_packet_create(message.data(), message.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(mPeer, 0, packet);

    enet_host_flush(mSocket);
}

void Ipc::receiveFromSecondaryInstance(function<void(const IpcPacket&)> callback) {
    if (!mIsPrimaryInstance) {
        throw runtime_error{"Must be the primary instance to receive from a secondary instance."};
    }

    ENetEvent event;
    while (enet_host_service(mSocket, &event, 0) > 0) {
        if (event.type != ENET_EVENT_TYPE_RECEIVE) {
            continue;
        }

        if (event.packet->dataLength <= 0) {
            continue;
        }

        IpcPacket packet{(char*)event.packet->data, (size_t)event.packet->dataLength};
        callback(packet);

        enet_packet_destroy(event.packet);
    }
}

TEV_NAMESPACE_END
