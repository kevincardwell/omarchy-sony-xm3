#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <deque>
#include <chrono>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// Constants & UUIDs
// ---------------------------------------------------------------------------
// The XM3 exposes the v1 ("legacy") MDR service. The v2 UUID is listed only so
// that an XM5-class headset can be recognised and rejected with a clear message
// rather than silently failing to speak the v1 command table at it.
constexpr const char* MDR_UUID_V1       = "96CC203E-5068-46AD-B32D-E316F5E069BA";
constexpr const char* MDR_UUID_V1_LOWER = "96cc203e-5068-46ad-b32d-e316f5e069ba";
constexpr const char* MDR_UUID_V2       = "956C7B26-D49A-4BA8-B03F-B17D393CB6E2";
constexpr const char* MDR_UUID_V2_LOWER = "956c7b26-d49a-4ba8-b03f-b17d393cb6e2";

// Used only when SDP cannot be reached; the real channel is resolved per device.
constexpr uint8_t DEFAULT_RFCOMM_CHANNEL = 9;

enum class ConnectionState {
    DISCONNECTED,
    DISCOVERING,
    RESOLVING_SDP,
    CONNECTING,
    CONNECTED,
    RECONNECT_BACKOFF
};

std::string connectionStateToString(ConnectionState state);

struct BluetoothDeviceInfo {
    std::string path;               // D-Bus object path (e.g. /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX)
    std::string macAddress;         // Colon-separated MAC address (e.g. 11:22:33:44:55:66)
    std::string name;               // Advertised device name (e.g. "WH-1000XM3")
    std::string alias;              // User-assigned alias
    bool paired{false};             // Whether device is paired in BlueZ
    bool connected{false};          // Whether underlying ACL link is connected
    int batteryLevel{-1};           // BlueZ battery percentage (-1 if unknown)
    std::vector<std::string> uuids; // Discovered service UUIDs
};

struct BluetoothConfig {
    std::string preferredMac;          // If set, only connect to this MAC
    bool autoReconnect{true};          // Automatically retry on drop/failure
    uint32_t initialBackoffMs{2000};   // Initial reconnect delay (2s)
    uint32_t maxBackoffMs{10000};      // Maximum reconnect delay (10s)
    float backoffMultiplier{1.5f};     // Exponential backoff multiplier
    uint32_t sdpTimeoutMs{3000};       // SDP service resolution timeout (ms)
    uint32_t connectTimeoutMs{5000};   // RFCOMM connect timeout (ms)
    uint8_t defaultChannel{DEFAULT_RFCOMM_CHANNEL}; // Fallback channel if SDP fails
    bool mockMode{false};              // Enable mock transport
};

// ---------------------------------------------------------------------------
// Abstract Interfaces
// ---------------------------------------------------------------------------

// Transport abstraction (RFCOMM socket or Mock UNIX socketpair)
class IBluetoothTransport {
public:
    virtual ~IBluetoothTransport() = default;

    // Connects to target MAC and channel.
    // Returns 0 if connected immediately, 1 if async in progress (EINPROGRESS), -1 on error.
    virtual int connect(const std::string& macAddress, uint8_t channel) = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual int getFd() const = 0;

    virtual ssize_t send(const uint8_t* data, size_t length) = 0;
    virtual ssize_t recv(uint8_t* buffer, size_t maxLength) = 0;

    // Inspects async connect result on POLLOUT. Returns 0 on success, errno on error.
    virtual int checkConnectResult() = 0;
    [[nodiscard]] virtual std::string getLastError() const = 0;
};

// Discovery abstraction (BlueZ D-Bus ObjectManager or Mock)
class IDeviceDiscovery {
public:
    virtual ~IDeviceDiscovery() = default;
    virtual std::vector<BluetoothDeviceInfo> getAvailableDevices() = 0;
    virtual std::optional<BluetoothDeviceInfo> findSonyHeadphones(const std::string& preferredMac = "") = 0;
};

// SDP Resolver abstraction (BlueZ sdp_lib or Mock)
class ISdpResolver {
public:
    virtual ~ISdpResolver() = default;
    virtual int resolveRfcommChannel(const std::string& macAddress,
                                     const std::string& uuid,
                                     uint32_t timeoutMs = 3000) = 0;
};

// ---------------------------------------------------------------------------
// Mock Transport Implementations (Exposed for Testing)
// ---------------------------------------------------------------------------

class MockTransport : public IBluetoothTransport {
public:
    explicit MockTransport(int* outPeerFd = nullptr);
    ~MockTransport() override;

    int connect(const std::string& macAddress, uint8_t channel) override;
    void disconnect() override;
    [[nodiscard]] bool isConnected() const override { return connected_; }
    [[nodiscard]] int getFd() const override { return fd_; }
    [[nodiscard]] int getPeerFd() const noexcept { return peerFd_; }

    ssize_t send(const uint8_t* data, size_t length) override;
    ssize_t recv(uint8_t* buffer, size_t maxLength) override;
    int checkConnectResult() override;
    [[nodiscard]] std::string getLastError() const override { return lastError_; }

    // Helper to simulate remote drop from test code
    void simulateRemoteDisconnect();

private:
    int fd_{-1};
    int peerFd_{-1};
    bool connected_{false};
    std::string lastError_;
};

class MockDeviceDiscovery : public IDeviceDiscovery {
public:
    std::vector<BluetoothDeviceInfo> devices;

    std::vector<BluetoothDeviceInfo> getAvailableDevices() override {
        return devices;
    }

    std::optional<BluetoothDeviceInfo> findSonyHeadphones(const std::string& preferredMac = "") override;
};

class MockSdpResolver : public ISdpResolver {
public:
    int channelToReturn{DEFAULT_RFCOMM_CHANNEL};
    int resolveRfcommChannel(const std::string& macAddress,
                             const std::string& uuid,
                             uint32_t timeoutMs = 3000) override {
        return channelToReturn;
    }
};

// ---------------------------------------------------------------------------
// Event Callbacks
// ---------------------------------------------------------------------------
struct BluetoothCallbacks {
    std::function<void()> onConnected;
    std::function<void(const std::string& reason)> onDisconnected;
    std::function<void(const uint8_t* data, size_t length)> onDataReceived;
    std::function<void(ConnectionState newState)> onStateChanged;
};

// ---------------------------------------------------------------------------
// BluetoothManager Subsystem
// ---------------------------------------------------------------------------
class BluetoothManager {
public:
    explicit BluetoothManager(BluetoothConfig config = {},
                              std::unique_ptr<IBluetoothTransport> transport = nullptr,
                              std::unique_ptr<IDeviceDiscovery> discovery = nullptr,
                              std::unique_ptr<ISdpResolver> sdpResolver = nullptr);
    ~BluetoothManager();

    // Factory methods
    static std::unique_ptr<BluetoothManager> createLinux(BluetoothConfig config = {});
    static std::unique_ptr<BluetoothManager> createMock(BluetoothConfig config = {},
                                                        int* outPeerFd = nullptr);

    void setCallbacks(BluetoothCallbacks callbacks);

    // Lifecycle
    void start();
    void stop();
    void disconnect();

    // Packet transmission
    bool sendPacket(const uint8_t* data, size_t length);
    bool sendPacket(const std::vector<uint8_t>& packet);

    // Event loop integration (poll/epoll)
    [[nodiscard]] int getPollFd() const;
    [[nodiscard]] short getPollEvents() const;
    void handleSocketEvent(short revents);
    void tick();

    // Inspection
    [[nodiscard]] ConnectionState getState() const noexcept { return state_; }
    [[nodiscard]] const BluetoothDeviceInfo& getCurrentDevice() const noexcept { return currentDevice_; }
    [[nodiscard]] uint8_t getCurrentChannel() const noexcept { return currentChannel_; }
    [[nodiscard]] std::string getLastError() const noexcept { return lastError_; }

private:
    void setState(ConnectionState newState);
    void handleConnecting(short revents);
    void handleConnectedRead();
    void handleConnectedWrite();
    void scheduleReconnect(const std::string& reason);
    void attemptDiscovery();
    void attemptSdp();
    void attemptConnect();

    BluetoothConfig config_;
    BluetoothCallbacks callbacks_;
    ConnectionState state_{ConnectionState::DISCONNECTED};
    BluetoothDeviceInfo currentDevice_;
    uint8_t currentChannel_{DEFAULT_RFCOMM_CHANNEL};
    std::string lastError_;

    std::unique_ptr<IBluetoothTransport> transport_;
    std::unique_ptr<IDeviceDiscovery> discovery_;
    std::unique_ptr<ISdpResolver> sdpResolver_;

    // Outbound queue for non-blocking partial writes
    std::deque<uint8_t> sendQueue_;

    // Timing & backoff
    uint32_t currentBackoffMs_{2000};
    uint32_t retryCount_{0};
    std::chrono::steady_clock::time_point lastStateChangeTime_;
    std::chrono::steady_clock::time_point nextReconnectTime_;
};

} // namespace omarchy::sony::protocol
