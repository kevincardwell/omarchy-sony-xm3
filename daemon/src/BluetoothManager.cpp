#include "BluetoothManager.hpp"

#include <sys/socket.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iostream>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <dbus/dbus.h>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string connectionStateToString(ConnectionState state) {
    switch (state) {
        case ConnectionState::DISCONNECTED:      return "DISCONNECTED";
        case ConnectionState::DISCOVERING:       return "DISCOVERING";
        case ConnectionState::RESOLVING_SDP:     return "RESOLVING_SDP";
        case ConnectionState::CONNECTING:        return "CONNECTING";
        case ConnectionState::CONNECTED:         return "CONNECTED";
        case ConnectionState::RECONNECT_BACKOFF: return "RECONNECT_BACKOFF";
        default:                                 return "UNKNOWN";
    }
}

static int parseUuidString(const char* szSrc, uint8_t* dst) {
    if (!szSrc || std::strlen(szSrc) != 36) return -1;
    unsigned int b[16];
    int ret = std::sscanf(szSrc,
        "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7],
        &b[8], &b[9], &b[10], &b[11], &b[12], &b[13], &b[14], &b[15]);
    if (ret != 16) return -1;
    for (int i = 0; i < 16; ++i) {
        dst[i] = static_cast<uint8_t>(b[i]);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Real Linux RFCOMM Transport
// ---------------------------------------------------------------------------

class RfcommTransport : public IBluetoothTransport {
public:
    RfcommTransport() : fd_(-1), connected_(false) {}
    ~RfcommTransport() override { disconnect(); }

    int connect(const std::string& macAddress, uint8_t channel) override {
        disconnect();

        fd_ = ::socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK, BTPROTO_RFCOMM);
        if (fd_ < 0) {
            lastError_ = "Failed to create RFCOMM socket: " + std::string(std::strerror(errno));
            return -1;
        }

        // Set security and encryption level (non-fatal if unsupported)
        unsigned int linkmode = RFCOMM_LM_AUTH | RFCOMM_LM_ENCRYPT;
        ::setsockopt(fd_, SOL_RFCOMM, RFCOMM_LM, &linkmode, sizeof(linkmode));

        struct sockaddr_rc addr{};
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_channel = channel;
        if (str2ba(macAddress.c_str(), &addr.rc_bdaddr) < 0) {
            lastError_ = "Invalid MAC address: " + macAddress;
            disconnect();
            return -1;
        }

        int res = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (res == 0) {
            connected_ = true;
            return 0; // Immediate connection
        }

        if (errno == EINPROGRESS) {
            connected_ = false;
            return 1; // Connecting asynchronously
        }

        lastError_ = "Connect failed: " + std::string(std::strerror(errno));
        disconnect();
        return -1;
    }

    void disconnect() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    [[nodiscard]] bool isConnected() const override { return connected_; }
    [[nodiscard]] int getFd() const override { return fd_; }

    ssize_t send(const uint8_t* data, size_t length) override {
        if (fd_ < 0 || !connected_) return -1;
        ssize_t n = ::send(fd_, data, length, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            lastError_ = "Send error: " + std::string(std::strerror(errno));
            return -1;
        }
        return n;
    }

    ssize_t recv(uint8_t* buffer, size_t maxLength) override {
        if (fd_ < 0 || !connected_) return -1;
        ssize_t n = ::recv(fd_, buffer, maxLength, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lastError_ = "Recv error: " + std::string(std::strerror(errno));
            }
            return -1;
        }
        return n;
    }

    int checkConnectResult() override {
        if (fd_ < 0) return ENOTCONN;
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
            return errno;
        }
        if (so_error == 0) {
            connected_ = true;
        } else {
            lastError_ = "Async connect failed: " + std::string(std::strerror(so_error));
        }
        return so_error;
    }

    [[nodiscard]] std::string getLastError() const override { return lastError_; }

private:
    int fd_{-1};
    bool connected_{false};
    std::string lastError_;
};

// ---------------------------------------------------------------------------
// BlueZ D-Bus Device Discovery
// ---------------------------------------------------------------------------

class DBusDeviceDiscovery : public IDeviceDiscovery {
public:
    DBusDeviceDiscovery() = default;

    std::vector<BluetoothDeviceInfo> getAvailableDevices() override {
        std::vector<BluetoothDeviceInfo> devices;
        DBusError error;
        dbus_error_init(&error);

        DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
        if (dbus_error_is_set(&error) || !conn) {
            dbus_error_free(&error);
            return devices;
        }

        DBusMessage* msg = dbus_message_new_method_call(
            "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        if (!msg) {
            dbus_connection_unref(conn);
            return devices;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &error);
        dbus_message_unref(msg);
        if (dbus_error_is_set(&error) || !reply) {
            dbus_error_free(&error);
            dbus_connection_unref(conn);
            return devices;
        }

        DBusMessageIter rootIter, dictIter;
        if (dbus_message_iter_init(reply, &rootIter) &&
            dbus_message_iter_get_arg_type(&rootIter) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&rootIter, &dictIter);

            while (dbus_message_iter_get_arg_type(&dictIter) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entryIter, ifacesIter;
                dbus_message_iter_recurse(&dictIter, &entryIter);

                char* objPath = nullptr;
                dbus_message_iter_get_basic(&entryIter, &objPath);
                dbus_message_iter_next(&entryIter);

                if (dbus_message_iter_get_arg_type(&entryIter) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&entryIter, &ifacesIter);

                    while (dbus_message_iter_get_arg_type(&ifacesIter) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter ifaceEntryIter, propsIter;
                        dbus_message_iter_recurse(&ifacesIter, &ifaceEntryIter);

                        char* ifaceName = nullptr;
                        dbus_message_iter_get_basic(&ifaceEntryIter, &ifaceName);
                        dbus_message_iter_next(&ifaceEntryIter);

                        if (ifaceName && std::strcmp(ifaceName, "org.bluez.Device1") == 0) {
                            BluetoothDeviceInfo dev;
                            dev.path = objPath ? objPath : "";

                            if (dbus_message_iter_get_arg_type(&ifaceEntryIter) == DBUS_TYPE_ARRAY) {
                                dbus_message_iter_recurse(&ifaceEntryIter, &propsIter);
                                parseDeviceProperties(&propsIter, dev);
                            }
                            devices.push_back(dev);
                        } else if (ifaceName && std::strcmp(ifaceName, "org.bluez.Battery1") == 0 && !devices.empty() && objPath && devices.back().path == objPath) {
                            if (dbus_message_iter_get_arg_type(&ifaceEntryIter) == DBUS_TYPE_ARRAY) {
                                dbus_message_iter_recurse(&ifaceEntryIter, &propsIter);
                                parseBatteryProperties(&propsIter, devices.back());
                            }
                        }
                        dbus_message_iter_next(&ifacesIter);
                    }
                }
                dbus_message_iter_next(&dictIter);
            }
        }

        dbus_message_unref(reply);
        dbus_connection_unref(conn);
        return devices;
    }

    std::optional<BluetoothDeviceInfo> findSonyHeadphones(const std::string& preferredMac = "") override {
        auto devices = getAvailableDevices();
        if (devices.empty()) return std::nullopt;

        // 1. Explicit MAC match if specified
        if (!preferredMac.empty()) {
            for (const auto& dev : devices) {
                if (strcasecmp(dev.macAddress.c_str(), preferredMac.c_str()) == 0) {
                    return dev;
                }
            }
            return std::nullopt;
        }

        // 2. Filter paired devices advertising the v1 MDR service, or named XM3.
        //    A device that advertises only the v2 service is deliberately not a
        //    candidate: it would accept the RFCOMM connection and then ignore
        //    every command, which looks like a much more confusing bug.
        std::vector<BluetoothDeviceInfo> candidates;
        for (const auto& dev : devices) {
            if (!dev.paired) continue;

            bool matchesV1Uuid = false;
            bool matchesV2Uuid = false;
            for (const auto& u : dev.uuids) {
                if (strcasecmp(u.c_str(), MDR_UUID_V1) == 0 ||
                    strcasecmp(u.c_str(), MDR_UUID_V1_LOWER) == 0) {
                    matchesV1Uuid = true;
                } else if (strcasecmp(u.c_str(), MDR_UUID_V2) == 0 ||
                           strcasecmp(u.c_str(), MDR_UUID_V2_LOWER) == 0) {
                    matchesV2Uuid = true;
                }
            }

            std::string nameLower = dev.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c){
                return static_cast<char>(std::tolower(c));
            });
            const bool matchesName = nameLower.find("wh-1000xm3") != std::string::npos;

            if (matchesV2Uuid && !matchesV1Uuid && !matchesName) {
                fprintf(stderr,
                        "[BT] Skipping %s (%s): advertises the v2 MDR service only. "
                        "This daemon speaks the v1 command table (WH-1000XM3).\n",
                        dev.name.c_str(), dev.macAddress.c_str());
                fflush(stderr);
                continue;
            }

            if (matchesV1Uuid || matchesName) {
                candidates.push_back(dev);
            }
        }

        if (candidates.empty()) return std::nullopt;

        // Prioritize device currently marked connected in BlueZ
        for (const auto& dev : candidates) {
            if (dev.connected) return dev;
        }

        return candidates.front();
    }

private:
    static void parseDeviceProperties(DBusMessageIter* propsIter, BluetoothDeviceInfo& dev) {
        while (dbus_message_iter_get_arg_type(propsIter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter propEntryIter, valIter;
            dbus_message_iter_recurse(propsIter, &propEntryIter);

            char* propName = nullptr;
            dbus_message_iter_get_basic(&propEntryIter, &propName);
            dbus_message_iter_next(&propEntryIter);

            if (propName && dbus_message_iter_get_arg_type(&propEntryIter) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&propEntryIter, &valIter);
                int valType = dbus_message_iter_get_arg_type(&valIter);
                std::string p(propName);

                if (p == "Name" && valType == DBUS_TYPE_STRING) {
                    char* s = nullptr;
                    dbus_message_iter_get_basic(&valIter, &s);
                    if (s) dev.name = s;
                } else if (p == "Alias" && valType == DBUS_TYPE_STRING) {
                    char* s = nullptr;
                    dbus_message_iter_get_basic(&valIter, &s);
                    if (s) dev.alias = s;
                } else if (p == "Address" && valType == DBUS_TYPE_STRING) {
                    char* s = nullptr;
                    dbus_message_iter_get_basic(&valIter, &s);
                    if (s) dev.macAddress = s;
                } else if (p == "Paired" && valType == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b;
                    dbus_message_iter_get_basic(&valIter, &b);
                    dev.paired = (b != 0);
                } else if (p == "Connected" && valType == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b;
                    dbus_message_iter_get_basic(&valIter, &b);
                    dev.connected = (b != 0);
                } else if (p == "UUIDs" && valType == DBUS_TYPE_ARRAY) {
                    DBusMessageIter arrIter;
                    dbus_message_iter_recurse(&valIter, &arrIter);
                    while (dbus_message_iter_get_arg_type(&arrIter) == DBUS_TYPE_STRING) {
                        char* u = nullptr;
                        dbus_message_iter_get_basic(&arrIter, &u);
                        if (u) dev.uuids.emplace_back(u);
                        dbus_message_iter_next(&arrIter);
                    }
                }
            }
            dbus_message_iter_next(propsIter);
        }
    }

    static void parseBatteryProperties(DBusMessageIter* propsIter, BluetoothDeviceInfo& dev) {
        while (dbus_message_iter_get_arg_type(propsIter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter propEntryIter, valIter;
            dbus_message_iter_recurse(propsIter, &propEntryIter);

            char* propName = nullptr;
            dbus_message_iter_get_basic(&propEntryIter, &propName);
            dbus_message_iter_next(&propEntryIter);

            if (propName && dbus_message_iter_get_arg_type(&propEntryIter) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&propEntryIter, &valIter);
                int valType = dbus_message_iter_get_arg_type(&valIter);
                std::string p(propName);

                if (p == "Percentage" && valType == DBUS_TYPE_BYTE) {
                    uint8_t pct = 0;
                    dbus_message_iter_get_basic(&valIter, &pct);
                    dev.batteryLevel = static_cast<int>(pct);
                }
            }
            dbus_message_iter_next(propsIter);
        }
    }
};

// ---------------------------------------------------------------------------
// BlueZ SDP Service Resolver
// ---------------------------------------------------------------------------

class BlueZSdpResolver : public ISdpResolver {
public:
    int resolveRfcommChannel(const std::string& macAddress,
                             const std::string& uuid,
                             uint32_t timeoutMs) override {
        uint8_t uuidBytes[16];
        if (parseUuidString(uuid.c_str(), uuidBytes) != 0) {
            return -1;
        }

        bdaddr_t target;
        if (str2ba(macAddress.c_str(), &target) < 0) {
            return -1;
        }

        const bdaddr_t bdaddr_any = {{0, 0, 0, 0, 0, 0}};
        sdp_session_t* session = sdp_connect(&bdaddr_any, &target, SDP_NON_BLOCKING);
        if (!session) {
            return -1;
        }

        int sdp_sock = sdp_get_socket(session);
        struct pollfd pfd{
            sdp_sock,
            POLLIN | POLLOUT,
            0
        };

        int poll_rc = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
        if (poll_rc <= 0 || (pfd.revents & (POLLERR | POLLHUP))) {
            sdp_close(session);
            return -1;
        }

        // Restore blocking mode for SDP query
        int flags = fcntl(sdp_sock, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(sdp_sock, F_SETFL, flags & ~O_NONBLOCK);
        }

        uuid_t svc_uuid;
        sdp_uuid128_create(&svc_uuid, uuidBytes);

        sdp_list_t* search_list = sdp_list_append(nullptr, &svc_uuid);
        uint32_t range = 0x0000ffff;
        sdp_list_t* attrid_list = sdp_list_append(nullptr, &range);
        sdp_list_t* response_list = nullptr;

        int status = sdp_service_search_attr_req(
            session, search_list, SDP_ATTR_REQ_RANGE, attrid_list, &response_list);

        uint8_t channel = 0;
        if (status == 0 && response_list) {
            for (sdp_list_t* r = response_list; r; r = r->next) {
                auto* rec = reinterpret_cast<sdp_record_t*>(r->data);
                sdp_list_t* proto_list = nullptr;
                if (sdp_get_access_protos(rec, &proto_list) == 0) {
                    int p = sdp_get_proto_port(proto_list, RFCOMM_UUID);
                    if (p > 0 && p <= 30) {
                        channel = static_cast<uint8_t>(p);
                    }
                    sdp_list_free(proto_list, nullptr);
                    if (channel > 0) break;
                }
                sdp_record_free(rec);
            }
        }

        if (response_list) sdp_list_free(response_list, nullptr);
        sdp_list_free(search_list, nullptr);
        sdp_list_free(attrid_list, nullptr);
        sdp_close(session);

        return channel > 0 ? static_cast<int>(channel) : -1;
    }
};

// ---------------------------------------------------------------------------
// Mock Implementations
// ---------------------------------------------------------------------------

MockTransport::MockTransport(int* outPeerFd) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0) {
        fd_ = sv[0];
        peerFd_ = sv[1];
        if (outPeerFd) *outPeerFd = peerFd_;
    }
}

MockTransport::~MockTransport() {
    disconnect();
}

int MockTransport::connect(const std::string&, uint8_t) {
    if (fd_ < 0) {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0) {
            fd_ = sv[0];
            peerFd_ = sv[1];
        } else {
            lastError_ = "Failed to allocate mock socketpair";
            return -1;
        }
    }
    connected_ = true;
    return 0; // Immediate connection in mock mode
}

void MockTransport::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (peerFd_ >= 0) {
        ::close(peerFd_);
        peerFd_ = -1;
    }
    connected_ = false;
}

void MockTransport::simulateRemoteDisconnect() {
    if (peerFd_ >= 0) {
        ::close(peerFd_);
        peerFd_ = -1;
    }
}

ssize_t MockTransport::send(const uint8_t* data, size_t length) {
    if (fd_ < 0 || !connected_) return -1;
    ssize_t n = ::send(fd_, data, length, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}

ssize_t MockTransport::recv(uint8_t* buffer, size_t maxLength) {
    if (fd_ < 0 || !connected_) return -1;
    ssize_t n = ::recv(fd_, buffer, maxLength, 0);
    if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
        lastError_ = "Recv error: " + std::string(std::strerror(errno));
    }
    return n;
}

int MockTransport::checkConnectResult() {
    connected_ = (fd_ >= 0);
    return connected_ ? 0 : ECONNREFUSED;
}

std::optional<BluetoothDeviceInfo> MockDeviceDiscovery::findSonyHeadphones(const std::string& preferredMac) {
    if (devices.empty()) {
        BluetoothDeviceInfo dev;
        dev.macAddress = "AA:BB:CC:DD:EE:FF";
        dev.name = "WH-1000XM3";
        dev.alias = "WH-1000XM3";
        dev.paired = true;
        dev.connected = true;
        dev.uuids.push_back(MDR_UUID_V1_LOWER);
        return dev;
    }

    if (!preferredMac.empty()) {
        for (const auto& dev : devices) {
            if (strcasecmp(dev.macAddress.c_str(), preferredMac.c_str()) == 0) {
                return dev;
            }
        }
        return std::nullopt;
    }

    return devices.front();
}

// ---------------------------------------------------------------------------
// BluetoothManager Implementation
// ---------------------------------------------------------------------------

BluetoothManager::BluetoothManager(BluetoothConfig config,
                                   std::unique_ptr<IBluetoothTransport> transport,
                                   std::unique_ptr<IDeviceDiscovery> discovery,
                                   std::unique_ptr<ISdpResolver> sdpResolver)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      discovery_(std::move(discovery)),
      sdpResolver_(std::move(sdpResolver)),
      currentBackoffMs_(config_.initialBackoffMs) {
    lastStateChangeTime_ = std::chrono::steady_clock::now();
}

BluetoothManager::~BluetoothManager() {
    stop();
}

std::unique_ptr<BluetoothManager> BluetoothManager::createLinux(BluetoothConfig config) {
    config.mockMode = false;
    auto transport = std::make_unique<RfcommTransport>();
    auto discovery = std::make_unique<DBusDeviceDiscovery>();
    auto sdp = std::make_unique<BlueZSdpResolver>();
    return std::make_unique<BluetoothManager>(
        std::move(config), std::move(transport), std::move(discovery), std::move(sdp));
}

std::unique_ptr<BluetoothManager> BluetoothManager::createMock(BluetoothConfig config, int* outPeerFd) {
    config.mockMode = true;
    auto transport = std::make_unique<MockTransport>(outPeerFd);
    auto discovery = std::make_unique<MockDeviceDiscovery>();
    auto sdp = std::make_unique<MockSdpResolver>();
    return std::make_unique<BluetoothManager>(
        std::move(config), std::move(transport), std::move(discovery), std::move(sdp));
}

void BluetoothManager::setCallbacks(BluetoothCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void BluetoothManager::setState(ConnectionState newState) {
    if (state_ != newState) {
        state_ = newState;
        lastStateChangeTime_ = std::chrono::steady_clock::now();
        if (callbacks_.onStateChanged) {
            callbacks_.onStateChanged(newState);
        }
    }
}

void BluetoothManager::start() {
    if (state_ == ConnectionState::CONNECTED || state_ == ConnectionState::CONNECTING) {
        return;
    }
    retryCount_ = 0;
    currentBackoffMs_ = config_.initialBackoffMs;
    attemptDiscovery();
}

void BluetoothManager::stop() {
    disconnect();
    setState(ConnectionState::DISCONNECTED);
}

void BluetoothManager::disconnect() {
    if (transport_) {
        transport_->disconnect();
    }
    sendQueue_.clear();
    setState(ConnectionState::DISCONNECTED);
}

void BluetoothManager::attemptDiscovery() {
    setState(ConnectionState::DISCOVERING);
    if (!discovery_) {
        scheduleReconnect("No discovery engine available");
        return;
    }

    auto devOpt = discovery_->findSonyHeadphones(config_.preferredMac);
    if (!devOpt.has_value()) {
        scheduleReconnect("No paired WH-1000XM3 found");
        return;
    }

    currentDevice_ = *devOpt;
    if (!currentDevice_.paired) {
        scheduleReconnect("Headphone device is not paired");
        return;
    }

    // In real mode, if BlueZ reports the device is not connected at the ACL link level,
    // skip wasteful RFCOMM connection attempts and wait in backoff.
    if (!config_.mockMode && !currentDevice_.connected) {
        scheduleReconnect("Headphones not currently connected via Bluetooth ACL");
        return;
    }

    attemptSdp();
}

void BluetoothManager::attemptSdp() {
    setState(ConnectionState::RESOLVING_SDP);

    // The RFCOMM channel carrying the MDR service is not fixed across firmware
    // revisions, so ask the device rather than assuming. config_.defaultChannel
    // is only a fallback for when SDP is unreachable.
    currentChannel_ = config_.defaultChannel;

    if (sdpResolver_ && !currentDevice_.macAddress.empty()) {
        int ch = sdpResolver_->resolveRfcommChannel(currentDevice_.macAddress,
                                                    MDR_UUID_V1,
                                                    config_.sdpTimeoutMs);
        if (ch > 0) {
            currentChannel_ = static_cast<uint8_t>(ch);
            fprintf(stderr, "[BT] SDP resolved MDR v1 service to RFCOMM channel %d\n", ch);
        } else {
            fprintf(stderr, "[BT] SDP lookup failed; falling back to channel %u\n",
                    (unsigned)currentChannel_);
        }
        fflush(stderr);
    }

    attemptConnect();
}

void BluetoothManager::attemptConnect() {
    setState(ConnectionState::CONNECTING);
    if (!transport_) {
        scheduleReconnect("No transport engine available");
        return;
    }

    fprintf(stderr, "[BT] Attempting RFCOMM connect to %s channel %u\n",
            currentDevice_.macAddress.c_str(), (unsigned)currentChannel_);
    fflush(stderr);
    int rc = transport_->connect(currentDevice_.macAddress, currentChannel_);
    if (rc == 0) {
        // Connected immediately
        fprintf(stderr, "[BT] RFCOMM connected immediately\n");
        fflush(stderr);
        setState(ConnectionState::CONNECTED);
        retryCount_ = 0;
        currentBackoffMs_ = config_.initialBackoffMs;
        if (callbacks_.onConnected) {
            callbacks_.onConnected();
        }
    } else if (rc == 1) {
        // Asynchronous connect in progress, waiting for POLLOUT
        fprintf(stderr, "[BT] RFCOMM async connect in progress...\n");
        fflush(stderr);
    } else {
        scheduleReconnect("RFCOMM connect initiation failed: " + transport_->getLastError());
    }
}

int BluetoothManager::getPollFd() const {
    if (!transport_) return -1;
    return transport_->getFd();
}

short BluetoothManager::getPollEvents() const {
    if (state_ == ConnectionState::CONNECTING) {
        return POLLOUT | POLLERR | POLLHUP;
    }
    if (state_ == ConnectionState::CONNECTED) {
        short events = POLLIN | POLLERR | POLLHUP;
        if (!sendQueue_.empty()) {
            events |= POLLOUT;
        }
        return events;
    }
    return 0;
}

void BluetoothManager::handleSocketEvent(short revents) {
    if (state_ == ConnectionState::CONNECTING) {
        handleConnecting(revents);
    } else if (state_ == ConnectionState::CONNECTED) {
        if (revents & (POLLERR | POLLHUP)) {
            scheduleReconnect("Socket hangup/error detected");
            return;
        }
        if (revents & POLLIN) {
            handleConnectedRead();
        }
        if (revents & POLLOUT) {
            handleConnectedWrite();
        }
    }
}

void BluetoothManager::handleConnecting(short revents) {
    if (revents & POLLOUT) {
        int err = transport_->checkConnectResult();
        if (err == 0) {
            fprintf(stderr, "[BT] RFCOMM async connect succeeded\n");
            fflush(stderr);
            setState(ConnectionState::CONNECTED);
            retryCount_ = 0;
            currentBackoffMs_ = config_.initialBackoffMs;
            if (callbacks_.onConnected) {
                callbacks_.onConnected();
            }
        } else {
            scheduleReconnect("Async connect check failed: " + transport_->getLastError());
        }
    } else if (revents & (POLLERR | POLLHUP)) {
        scheduleReconnect("Async connect socket hangup/error");
    }
}

void BluetoothManager::handleConnectedRead() {
    uint8_t buf[1024];
    ssize_t n = transport_->recv(buf, sizeof(buf));
    if (n > 0) {
        if (callbacks_.onDataReceived) {
            callbacks_.onDataReceived(buf, static_cast<size_t>(n));
        }
    } else if (n == 0) {
        scheduleReconnect("Remote peer closed RFCOMM connection");
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        scheduleReconnect("RFCOMM read error: " + transport_->getLastError());
    }
}

void BluetoothManager::handleConnectedWrite() {
    while (!sendQueue_.empty()) {
        std::vector<uint8_t> chunk(sendQueue_.begin(), sendQueue_.end());
        ssize_t n = transport_->send(chunk.data(), chunk.size());
        if (n > 0) {
            sendQueue_.erase(sendQueue_.begin(), sendQueue_.begin() + n);
        } else if (n == 0) {
            break; // Socket buffer full, wait for next POLLOUT
        } else {
            scheduleReconnect("RFCOMM send failed: " + transport_->getLastError());
            break;
        }
    }
}

bool BluetoothManager::sendPacket(const uint8_t* data, size_t length) {
    if (state_ != ConnectionState::CONNECTED || !transport_) {
        return false;
    }

    if (sendQueue_.empty()) {
        ssize_t n = transport_->send(data, length);
        if (n >= 0 && static_cast<size_t>(n) == length) {
            return true;
        }
        if (n > 0) {
            sendQueue_.insert(sendQueue_.end(), data + n, data + length);
            return true;
        }
        if (n == 0) {
            sendQueue_.insert(sendQueue_.end(), data, data + length);
            return true;
        }
        scheduleReconnect("Send error: " + transport_->getLastError());
        return false;
    }

    sendQueue_.insert(sendQueue_.end(), data, data + length);
    return true;
}

bool BluetoothManager::sendPacket(const std::vector<uint8_t>& packet) {
    return sendPacket(packet.data(), packet.size());
}

void BluetoothManager::scheduleReconnect(const std::string& reason) {
    lastError_ = reason;
    fprintf(stderr, "[BT] Reconnect scheduled: %s\n", reason.c_str());
    fflush(stderr);
    if (transport_) {
        transport_->disconnect();
    }
    sendQueue_.clear();
    setState(ConnectionState::RECONNECT_BACKOFF);

    if (callbacks_.onDisconnected) {
        callbacks_.onDisconnected(reason);
    }

    if (!config_.autoReconnect) {
        setState(ConnectionState::DISCONNECTED);
        return;
    }

    retryCount_++;
    auto delay = static_cast<uint32_t>(config_.initialBackoffMs *
                 std::pow(config_.backoffMultiplier, std::min(retryCount_, 10u)));
    currentBackoffMs_ = std::min(delay, config_.maxBackoffMs);
    nextReconnectTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(currentBackoffMs_);
    fprintf(stderr, "[BT] Will retry in %u ms (attempt #%u)\n", currentBackoffMs_, retryCount_);
    fflush(stderr);
}

void BluetoothManager::tick() {
    auto now = std::chrono::steady_clock::now();

    // Check connect timeout
    if (state_ == ConnectionState::CONNECTING) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStateChangeTime_).count();
        if (elapsed > static_cast<int64_t>(config_.connectTimeoutMs)) {
            scheduleReconnect("RFCOMM connection timed out");
            return;
        }
    }

    // Check reconnect backoff timer
    if (state_ == ConnectionState::RECONNECT_BACKOFF && config_.autoReconnect) {
        if (now >= nextReconnectTime_) {
            attemptDiscovery();
        }
    }
}

} // namespace omarchy::sony::protocol
