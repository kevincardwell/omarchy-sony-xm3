#pragma once

#include "MDRProtocolV1.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include <span>
#include <memory>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <sys/poll.h>

namespace omarchy::sony {

struct ClientSession {
    int fd{-1};
    std::string inBuffer;
    std::string outBuffer;
    std::chrono::steady_clock::time_point connectedAt;
    // Set by the `subscribe` command: the session is sent every status change.
    bool subscribed{false};
};

// Delegate callbacks for decoupling IpcServer from StateEngine and BluetoothManager
struct IpcCallbacks {
    std::function<std::string()> getStatusJson;
    std::function<bool(protocol::NoiseMode mode, uint8_t ambientLevel, std::string& errorMsg)> setNoiseMode;
    std::function<bool(uint8_t level, std::string& errorMsg)> setAmbientLevel;
    std::function<bool(protocol::EqPreset preset, std::string& errorMsg)> setEqPreset;
    std::function<bool(protocol::EqPreset slot, const std::array<int, 5>& bands, int clearBass,
                       std::string& errorMsg)> setCustomEq;
    std::function<bool(bool enabled, std::string& errorMsg)> setVoiceFocus;
    std::function<bool(bool enabled, std::string& errorMsg)> setDsee;
    std::function<bool(protocol::SurroundPreset preset, std::string& errorMsg)> setSurround;
    std::function<bool(protocol::SoundPosition position, std::string& errorMsg)> setSoundPosition;
    std::function<bool(protocol::AutoPowerOff timer, std::string& errorMsg)> setAutoPowerOff;
    std::function<bool(protocol::ConnectionMode mode, std::string& errorMsg)> setConnectionMode;
    std::function<bool(bool start, std::string& errorMsg)> setOptimizer;
    std::function<bool(uint8_t volume, std::string& errorMsg)> setVolume;
    std::function<bool(protocol::PlaybackControl control, std::string& errorMsg)> setPlayback;
    std::function<bool(protocol::NcButton button, std::string& errorMsg)> setNcButton;
    std::function<bool(bool enabled, std::string& errorMsg)> setTouchPanel;
    std::function<bool(bool enabled, std::string& errorMsg)> setVoiceGuidance;
    std::function<int()> getVolumeMax;
    std::function<bool(const std::vector<uint8_t>& packet)> sendPacket;
    // Upper bound the daemon will accept for `ambient-level`, learned from the
    // headset's NCASM capability response.
    std::function<int()> getAmbientMaxLevel;

    // Offline simulation test helper hooks
    std::function<void(int level, bool charging)> onTestSetBattery;
    std::function<void()> onTestDisconnect;
    std::function<void()> onTestReconnect;
};

class IpcServer {
public:
    using CommandHandler = std::function<std::string(const std::string& commandLine)>;

    explicit IpcServer(std::string socketPath = "");
    ~IpcServer();

    // Non-copyable, movable
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) noexcept;
    IpcServer& operator=(IpcServer&&) noexcept;

    // Path resolution utility
    [[nodiscard]] static std::string resolveSocketPath(const std::string& overridePath = "");

    // Adopts the listening socket systemd passed in (LISTEN_FDS), if any.
    // Returns the descriptor, or -1 when the daemon was not socket-activated.
    // The environment variables are cleared either way, so a descriptor is
    // adopted at most once.
    [[nodiscard]] static int takeSystemdListenFd();

    // True when the listening socket came from the service manager rather than
    // from bind() in this process.
    [[nodiscard]] bool isSocketActivated() const noexcept { return socketActivated_; }

    // Lifecycle
    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }

    // Handlers & Callbacks
    void setCommandHandler(CommandHandler handler) { customHandler_ = std::move(handler); }
    void setCallbacks(IpcCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    // Inspection
    [[nodiscard]] const std::string& getSocketPath() const noexcept { return actualSocketPath_; }
    [[nodiscard]] int getListenFd() const noexcept { return listenFd_; }
    [[nodiscard]] size_t getClientCount() const noexcept { return clients_.size(); }

    // Sends one status line to every subscribed client. A client that cannot
    // keep up (its queue passes maxOutBuffer_) is dropped rather than allowed
    // to grow the daemon's memory.
    void broadcastStatus(const std::string& statusJson);
    [[nodiscard]] size_t getSubscriberCount() const noexcept;

    // Command parser (public for unit testing without sockets)
    [[nodiscard]] std::string handleCommandLine(const std::string& line);
    [[nodiscard]] std::string handleBuiltinCommand(const std::string& line);

    // Event loop integration (poll/epoll)
    void appendPollFds(std::vector<struct pollfd>& pfds) const;
    void populatePollFds(std::vector<struct pollfd>& pfds) const { appendPollFds(pfds); }
    void handleSocketEvent(int fd, short revents);
    void handlePollEvents(std::span<const struct pollfd> pfds);

    // Standalone polling helper
    int pollOnce(int timeoutMs = 0);

private:
    void acceptClients();
    void handleClientRead(int clientFd);
    void handleClientWrite(int clientFd);
    void closeClient(int clientFd);
    bool queueResponse(ClientSession& session, const std::string& response);
    bool flushClientOutBuffer(ClientSession& session);

    uint8_t nextSeq() noexcept { return seq_++; }

    std::string socketPathConfig_;
    std::string actualSocketPath_;
    int listenFd_{-1};
    bool running_{false};
    bool socketActivated_{false};
    uint8_t seq_{0};
    size_t maxLineLength_{4096};
    size_t maxOutBuffer_{262144};

    CommandHandler customHandler_;
    IpcCallbacks callbacks_;
    std::unordered_map<int, ClientSession> clients_;
};

} // namespace omarchy::sony

namespace omarchy::sony::protocol {
    using IpcServer = omarchy::sony::IpcServer;
    using IpcCallbacks = omarchy::sony::IpcCallbacks;
    using ClientSession = omarchy::sony::ClientSession;
}
