#include "BluetoothManager.hpp"

#include <sys/socket.h>
#include <sys/poll.h>
#include <arpa/inet.h>
#include <unistd.h>
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
    DBusDeviceDiscovery() { ensureBus(); }

    ~DBusDeviceDiscovery() override {
        if (conn_) {
            dbus_connection_close(conn_);
            dbus_connection_unref(conn_);
        }
    }

    [[nodiscard]] int eventFd() const override {
        int fd = -1;
        if (conn_ && dbus_connection_get_is_connected(conn_)) {
            dbus_connection_get_unix_fd(conn_, &fd);
        }
        return fd;
    }

    bool drainEvents() override {
        changed_ = false;
        if (conn_) {
            dbus_connection_read_write(conn_, 0);
            dispatchAll();
        }
        return changed_;
    }

    bool setDeviceConnected(const std::string& path, bool connect, std::string& err) override {
        if (!ensureBus()) {
            err = "Cannot reach the system D-Bus";
            return false;
        }
        DBusMessage* msg = dbus_message_new_method_call(
            "org.bluez", path.c_str(), "org.bluez.Device1", connect ? "Connect" : "Disconnect");
        DBusPendingCall* pending = nullptr;
        // Paging a headset that is off takes BlueZ several seconds to give up on.
        const bool sent = msg && dbus_connection_send_with_reply(conn_, msg, &pending, 30000) && pending;
        if (msg) dbus_message_unref(msg);
        if (!sent) {
            err = "Cannot talk to BlueZ";
            return false;
        }
        linkResult_.reset();
        dbus_pending_call_set_notify(pending, &DBusDeviceDiscovery::onLinkReply, this, nullptr);
        dbus_pending_call_unref(pending);
        dbus_connection_flush(conn_);
        return true;
    }

    std::optional<std::string> takeLinkResult() override {
        auto result = std::move(linkResult_);
        linkResult_.reset();
        return result;
    }

    std::vector<BluetoothDeviceInfo> getAvailableDevices() override {
        std::vector<BluetoothDeviceInfo> devices;
        DBusError error;
        dbus_error_init(&error);

        if (!ensureBus()) {
            return devices;
        }
        DBusConnection* conn = conn_;

        DBusMessage* msg = dbus_message_new_method_call(
            "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        if (!msg) {
            return devices;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &error);
        dbus_message_unref(msg);
        // Signals that arrived while waiting are older than this answer, which
        // already reflects them; dispatch them so they do not sit in the queue.
        dispatchAll();
        if (dbus_error_is_set(&error) || !reply) {
            dbus_error_free(&error);
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
    // One private connection for the daemon's lifetime: it carries the signal
    // subscriptions as well as the calls. Reconnected if the bus goes away.
    bool ensureBus() {
        if (conn_ && dbus_connection_get_is_connected(conn_)) return true;
        if (conn_) {
            dbus_connection_unref(conn_);
            conn_ = nullptr;
        }
        DBusError error;
        dbus_error_init(&error);
        conn_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
        if (!conn_) {
            dbus_error_free(&error);
            return false;
        }
        // libdbus would otherwise _exit() the daemon if the system bus drops.
        dbus_connection_set_exit_on_disconnect(conn_, FALSE);
        dbus_connection_add_filter(conn_, &DBusDeviceDiscovery::onSignal, this, nullptr);
        // Only the changes that matter: a device's Connected or Paired flag
        // (checked in onSignal), and bluetoothd coming or going.
        dbus_bus_add_match(conn_,
            "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',arg0='org.bluez.Device1'", &error);
        dbus_error_free(&error);
        dbus_bus_add_match(conn_,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
            "member='NameOwnerChanged',arg0='org.bluez'", &error);
        dbus_error_free(&error);
        return true;
    }

    void dispatchAll() {
        while (conn_ && dbus_connection_dispatch(conn_) == DBUS_DISPATCH_DATA_REMAINS) {}
    }

    static DBusHandlerResult onSignal(DBusConnection*, DBusMessage* msg, void* data) {
        auto* self = static_cast<DBusDeviceDiscovery*>(data);
        if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
            self->changed_ = true;
        } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
            // (s interface, a{sv} changed, as invalidated): RSSI and the like
            // change constantly during a scan, so look for the two that matter.
            DBusMessageIter args, dict;
            if (dbus_message_iter_init(msg, &args) && dbus_message_iter_next(&args) &&
                dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&args, &dict);
                while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter entry;
                    dbus_message_iter_recurse(&dict, &entry);
                    const char* key = nullptr;
                    dbus_message_iter_get_basic(&entry, &key);
                    if (key && (std::strcmp(key, "Connected") == 0 || std::strcmp(key, "Paired") == 0)) {
                        self->changed_ = true;
                    }
                    dbus_message_iter_next(&dict);
                }
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    static void onLinkReply(DBusPendingCall* pending, void* data) {
        auto* self = static_cast<DBusDeviceDiscovery*>(data);
        DBusMessage* reply = dbus_pending_call_steal_reply(pending);
        std::string result;
        if (!reply) {
            result = "Bluetooth did not answer";
        } else if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            result = describeLinkError(dbus_message_get_error_name(reply), reply);
        }
        if (reply) dbus_message_unref(reply);
        self->linkResult_ = result;
    }

    // BlueZ's errors, in words someone at the panel can act on.
    static std::string describeLinkError(const char* name, DBusMessage* reply) {
        const std::string error = name ? name : "";
        const char* text = nullptr;
        dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
        std::string detail = text ? text : "";
        while (!detail.empty() && std::isspace(static_cast<unsigned char>(detail.back()))) detail.pop_back();
        std::string lower = detail;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (error == "org.bluez.Error.AlreadyConnected" || error == "org.bluez.Error.InProgress" ||
            error == "org.bluez.Error.NotConnected") {
            return "";
        }
        if (error == "org.bluez.Error.NotReady") {
            return "Bluetooth is off";
        }
        if (lower.find("page-timeout") != std::string::npos || lower.find("page timeout") != std::string::npos ||
            lower.find("host is down") != std::string::npos || lower.find("timed out") != std::string::npos) {
            return "The headset didn't answer. Switch it on, or disconnect it from your phone first";
        }
        return "Bluetooth: " + (detail.empty() ? error : detail);
    }

    DBusConnection* conn_{nullptr};
    bool changed_{false};
    std::optional<std::string> linkResult_;

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
        // Connecting and every round of the query share this one deadline.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
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
        // The query gets its own copy of the connected socket; sdp_close() closes the original.
        int fd = (poll_rc > 0 && !(pfd.revents & (POLLERR | POLLHUP))) ? ::dup(sdp_sock) : -1;
        sdp_close(session);
        return fd < 0 ? -1 : querySdpRfcommChannel(fd, uuid, deadline);
    }
};

// libbluetooth concatenates SDP continuation fragments with no total limit, so
// every reply PDU is sized from its header before sdp_process() may read it.
// A real MDR record set is a few hundred bytes.
constexpr size_t kSdpMaxResponseBytes = 64 * 1024;

int querySdpRfcommChannel(int fd, const std::string& uuid,
                          std::chrono::steady_clock::time_point deadline) {
    uint8_t uuidBytes[16];
    sdp_session_t* session = parseUuidString(uuid.c_str(), uuidBytes) == 0 ? sdp_create(fd, 0) : nullptr;
    if (!session) {
        ::close(fd);
        return -1;
    }

    struct Result { bool done = false; int channel = -1; } result;
    sdp_set_notify(session, [](uint8_t type, uint16_t status, uint8_t* rsp, size_t size, void* udata) {
        auto* r = static_cast<Result*>(udata);
        r->done = true;
        if (status != 0 || type != SDP_SVC_SEARCH_ATTR_RSP || !rsp) return;

        uint8_t dtd = 0;
        int seqlen = 0;
        int left = static_cast<int>(size);
        int scanned = sdp_extract_seqtype(rsp, left, &dtd, &seqlen);
        if (!scanned || !seqlen) return;
        rsp += scanned;
        left -= scanned;
        while (left > 0 && r->channel < 0) {
            int recsize = 0;
            sdp_record_t* rec = sdp_extract_pdu(rsp, left, &recsize);
            if (!rec) break;
            sdp_list_t* protos = nullptr;
            if (sdp_get_access_protos(rec, &protos) == 0) {
                int p = sdp_get_proto_port(protos, RFCOMM_UUID);
                if (p > 0 && p <= 30) r->channel = p;
                sdp_list_foreach(protos, reinterpret_cast<sdp_list_func_t>(sdp_list_free), nullptr);
                sdp_list_free(protos, nullptr);
            }
            sdp_record_free(rec);
            if (recsize <= 0) break;
            rsp += recsize;
            left -= recsize;
        }
    }, &result);

    uuid_t svcUuid;
    sdp_uuid128_create(&svcUuid, uuidBytes);
    uint32_t range = 0x0000ffff;
    sdp_list_t* search = sdp_list_append(nullptr, &svcUuid);
    sdp_list_t* attrs = sdp_list_append(nullptr, &range);
    bool sent = sdp_service_search_attr_async(session, search, SDP_ATTR_REQ_RANGE, attrs) == 0;
    sdp_list_free(search, nullptr);
    sdp_list_free(attrs, nullptr);

    size_t received = 0;
    while (sent && !result.done) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        struct pollfd pfd{fd, POLLIN, 0};
        if (left <= 0 || ::poll(&pfd, 1, static_cast<int>(left)) <= 0) break;

        sdp_pdu_hdr_t hdr;
        if (::recv(fd, &hdr, sizeof hdr, MSG_PEEK) != static_cast<ssize_t>(sizeof hdr)) break;
        received += sizeof hdr + ntohs(hdr.plen);
        if (received > kSdpMaxResponseBytes || sdp_process(session) != 0) break;
    }

    sdp_close(session);
    return result.channel;
}

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
    paired_ = devOpt.has_value() && devOpt->paired;
    if (!paired_) {
        waitForBluez("No paired WH-1000XM3 found");
        return;
    }
    currentDevice_ = *devOpt;

    // Until BlueZ has the headset connected there is no RFCOMM channel to open,
    // so wait for it to say so rather than knocking.
    if (!config_.mockMode && !currentDevice_.connected) {
        waitForBluez("Headset is not connected to this computer");
        return;
    }

    attemptSdp();
}

// Nothing can happen until BlueZ reports a change, so nothing is scheduled:
// the BlueZ signal wakes the event loop. Only without a bus to listen on does
// this fall back to asking again every few seconds.
void BluetoothManager::waitForBluez(const std::string& reason) {
    if (reason != waitReason_) {
        waitReason_ = reason;
        fprintf(stderr, "[BT] Waiting: %s\n", reason.c_str());
        fflush(stderr);
    }
    if (discovery_ && discovery_->eventFd() >= 0) {
        setState(ConnectionState::DISCONNECTED);
        return;
    }
    setState(ConnectionState::RECONNECT_BACKOFF);
    nextReconnectTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.maxBackoffMs);
}

void BluetoothManager::linkUp() {
    setState(ConnectionState::CONNECTED);
    retryCount_ = 0;
    currentBackoffMs_ = config_.initialBackoffMs;
    waitReason_.clear();
    if (linkState_ == "connecting") linkState_.clear();
    linkError_.clear();
    if (callbacks_.onConnected) {
        callbacks_.onConnected();
    }
}

bool BluetoothManager::requestLink(bool connect, std::string& err) {
    if (config_.mockMode) {
        if (connect) {
            start();
        } else if (state_ == ConnectionState::CONNECTED) {
            transport_->disconnect();
            sendQueue_.clear();
            setState(ConnectionState::DISCONNECTED);
            if (callbacks_.onDisconnected) callbacks_.onDisconnected("Disconnected on request");
        }
        return true;
    }

    auto devOpt = discovery_ ? discovery_->findSonyHeadphones(config_.preferredMac) : std::nullopt;
    paired_ = devOpt.has_value() && devOpt->paired;
    if (!paired_) {
        err = "No paired WH-1000XM3 found. Pair it in the Bluetooth settings first";
        return false;
    }
    currentDevice_ = *devOpt;

    if (connect && currentDevice_.connected) {
        // BlueZ already has it; only the control channel is missing.
        if (state_ == ConnectionState::DISCONNECTED || state_ == ConnectionState::RECONNECT_BACKOFF) {
            attemptDiscovery();
        }
        return true;
    }
    if (!connect && !currentDevice_.connected) {
        return true;
    }

    if (!discovery_->setDeviceConnected(currentDevice_.path, connect, err)) {
        return false;
    }
    fprintf(stderr, "[BT] Asked BlueZ to %s %s\n", connect ? "connect" : "disconnect",
            currentDevice_.macAddress.c_str());
    fflush(stderr);
    linkState_ = connect ? "connecting" : "disconnecting";
    linkError_.clear();
    linkDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    return true;
}

void BluetoothManager::handleBusEvent() {
    if (!discovery_) return;
    const bool changed = discovery_->drainEvents();

    if (discovery_->eventFd() < 0 && state_ == ConnectionState::DISCONNECTED) {
        // The bus went away while we were waiting on it: poll until it is back.
        waitForBluez(waitReason_);
    } else if (changed && (state_ == ConnectionState::DISCONNECTED ||
                           state_ == ConnectionState::RECONNECT_BACKOFF)) {
        retryCount_ = 0;
        currentBackoffMs_ = config_.initialBackoffMs;
        attemptDiscovery();
    }
    consumeLinkResult();
}

// BlueZ's answer to requestLink(). It can be read here or during any other
// BlueZ call (a device lookup dispatches whatever arrived with it), so it is
// collected after both.
void BluetoothManager::consumeLinkResult() {
    auto result = discovery_ ? discovery_->takeLinkResult() : std::nullopt;
    if (!result) return;
    if (!result->empty()) {
        fprintf(stderr, "[BT] %s failed: %s\n",
                linkState_ == "disconnecting" ? "Disconnect" : "Connect", result->c_str());
        fflush(stderr);
        linkError_ = *result;
        linkState_.clear();
    } else if (linkState_ == "disconnecting") {
        linkState_.clear();
    }
    // A connect that succeeded stays "connecting" until the control channel
    // is open too, so the panel does not flash "disconnected".
}

std::optional<std::chrono::steady_clock::time_point> BluetoothManager::deadline() const {
    std::optional<std::chrono::steady_clock::time_point> due;
    if (state_ == ConnectionState::CONNECTING) {
        due = lastStateChangeTime_ + std::chrono::milliseconds(config_.connectTimeoutMs);
    } else if (state_ == ConnectionState::RECONNECT_BACKOFF && config_.autoReconnect) {
        due = nextReconnectTime_;
    }
    if (!linkState_.empty() && (!due || linkDeadline_ < *due)) {
        due = linkDeadline_;
    }
    return due;
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
        } else {
            fprintf(stderr, "[BT] SDP lookup failed; falling back to channel %u\n",
                    (unsigned)currentChannel_);
            fflush(stderr);
        }
    }

    attemptConnect();
}

void BluetoothManager::attemptConnect() {
    setState(ConnectionState::CONNECTING);
    if (!transport_) {
        scheduleReconnect("No transport engine available");
        return;
    }

    int rc = transport_->connect(currentDevice_.macAddress, currentChannel_);
    if (rc == 0) {
        linkUp();
    } else if (rc == 1) {
        // Asynchronous connect in progress, waiting for POLLOUT
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
            linkUp();
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

// Every frame the headset sends is ACKed, so a peer that keeps talking while
// withholding RFCOMM credits would grow the queue without end. Real traffic
// queues a few frames at most; past the cap the link is dropped.
constexpr size_t kMaxSendQueueBytes = 64 * 1024;

bool BluetoothManager::sendPacket(const uint8_t* data, size_t length) {
    if (state_ != ConnectionState::CONNECTED || !transport_) {
        return false;
    }

    size_t sent = 0;
    if (sendQueue_.empty()) {
        ssize_t n = transport_->send(data, length);
        if (n < 0) {
            scheduleReconnect("Send error: " + transport_->getLastError());
            return false;
        }
        sent = static_cast<size_t>(n);
    }

    if (sendQueue_.size() + (length - sent) > kMaxSendQueueBytes) {
        scheduleReconnect("Headset stopped reading: send queue over 64 KiB");
        return false;
    }
    sendQueue_.insert(sendQueue_.end(), data + sent, data + length);
    return true;
}

bool BluetoothManager::sendPacket(const std::vector<uint8_t>& packet) {
    return sendPacket(packet.data(), packet.size());
}

// A connection attempt failed or an open link dropped. Only the drop of an
// open link is a disconnect; failed attempts just retry with backoff, and only
// for as long as BlueZ still has the headset (attemptDiscovery checks).
void BluetoothManager::scheduleReconnect(const std::string& reason) {
    const bool wasConnected = state_ == ConnectionState::CONNECTED;
    lastError_ = reason;
    if (transport_) {
        transport_->disconnect();
    }
    sendQueue_.clear();
    setState(ConnectionState::RECONNECT_BACKOFF);
    waitReason_.clear();

    if (wasConnected && callbacks_.onDisconnected) {
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
    fprintf(stderr, "[BT] %s; retrying in %u ms\n", reason.c_str(), currentBackoffMs_);
    fflush(stderr);
}

void BluetoothManager::tick() {
    auto now = std::chrono::steady_clock::now();
    consumeLinkResult();

    if (!linkState_.empty() && now >= linkDeadline_) {
        if (linkState_ == "connecting" && state_ != ConnectionState::CONNECTED) {
            linkError_ = "The headset connected, but its control channel did not open";
        }
        linkState_.clear();
    }

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
