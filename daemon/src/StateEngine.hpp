#pragma once

#include "MDRProtocolV1.hpp"
#include <string>
#include <filesystem>
#include <mutex>
#include <functional>
#include <optional>
#include <vector>
#include <array>
#include <span>

namespace omarchy::sony {

// State modification listener callback
using StateListener = std::function<void(const protocol::HeadphoneState&)>;

class StateEngine {
public:
    // Constructor accepts an optional custom state directory or file path.
    // If empty, standard XDG path resolution is performed.
    explicit StateEngine(const std::filesystem::path& customStatePath = "");
    ~StateEngine();

    // Non-copyable, non-movable
    StateEngine(const StateEngine&) = delete;
    StateEngine& operator=(const StateEngine&) = delete;
    StateEngine(StateEngine&&) = delete;
    StateEngine& operator=(StateEngine&&) = delete;

    // -----------------------------------------------------------------------
    // Path Resolution & Filesystem Lifecycle
    // -----------------------------------------------------------------------
    static std::filesystem::path resolveStateFilePath(const std::filesystem::path& customStatePath = "");
    static bool ensureStateDirectory(const std::filesystem::path& dirPath);

    // Initializer: creates directories and commits initial status.json
    bool initialize(bool initialConnected = true);
    bool init() { return initialize(true); }

    // Shutdown cleanup: unlinks status.json and any stale temporary files
    void cleanup();

    // -----------------------------------------------------------------------
    // Atomic Persistence Engine
    // -----------------------------------------------------------------------
    // Writes JSON content to <status.json>.tmp.<pid> with mode 0600,
    // calls fsync(), closes, and renames atomically to <status.json>.
    bool writeAtomic(const std::string& jsonContent);

    // Formats the current state and writes to disk atomically
    bool commit();
    bool save() { return commit(); }

    // -----------------------------------------------------------------------
    // State Accessors (Thread-Safe)
    // -----------------------------------------------------------------------
    [[nodiscard]] protocol::HeadphoneState getState() const;
    [[nodiscard]] protocol::HeadphoneState& getStateUnsafe() noexcept { return state_; }
    [[nodiscard]] std::string getStatusJson() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] const std::filesystem::path& getStateFilePath() const noexcept { return stateFilePath_; }
    [[nodiscard]] std::string getStatusFilePath() const { return stateFilePath_.string(); }
    [[nodiscard]] const std::filesystem::path& getStateDirectory() const noexcept { return stateDir_; }

    // -----------------------------------------------------------------------
    // Inbound Protocol Integration
    // -----------------------------------------------------------------------
    bool updateFromInbound(std::span<const uint8_t> payload);

    // -----------------------------------------------------------------------
    // State Mutators
    // -----------------------------------------------------------------------
    void setConnected(bool connected, const std::string& deviceName = "WH-1000XM3");
    void setDeviceName(const std::string& name);
    void setBatteryLevel(int level);
    void setCharging(bool charging);
    void setBattery(int level, bool charging);

    bool setNoiseMode(const std::string& mode);
    bool updateNoiseMode(const std::string& mode, int ambientLevel = 0, bool voiceFocus = false);

    bool setAmbientLevel(int level);
    bool updateAmbientLevel(int level) { return setAmbientLevel(level); }
    void setAmbientMaxLevel(int maxLevel);

    bool setVoicePassthrough(bool passthrough);

    bool setEqPreset(const std::string& preset);
    bool updateEqPreset(const std::string& preset) { return setEqPreset(preset); }

    bool setCustomEq(const std::array<int, 5>& bands, int clearBass);
    bool updateCustomEq(const std::array<int, 5>& bands, int clearBass) { return setCustomEq(bands, clearBass); }

    // DSEE HX upscaling (the XM3's equivalent of the XM4/XM5 "DSEE Extreme").
    void setDsee(bool enabled);
    void updateDsee(bool enabled) { setDsee(enabled); }

    // Pause playback when the headphones are taken off.
    void setEarDetection(bool enabled);
    void updateEarDetection(bool enabled) { setEarDetection(enabled); }

    // VPT surround preset: off | outdoor | arena | concert | club
    bool setSurround(const std::string& preset);
    // VPT sound position: off | front-left | front-right | front | rear-left | rear-right
    bool setSoundPosition(const std::string& position);
    // Auto power off: off | 5min | 30min | 60min | 180min | on-remove
    bool setAutoPowerOff(const std::string& timer);
    // Bluetooth link preference: quality | stable
    bool setConnectionMode(const std::string& mode);

    void setCodec(const std::string& codec);

    // Transactional mutation
    void modifyState(const std::function<void(protocol::HeadphoneState&)>& mutator);
    void setState(const protocol::HeadphoneState& newState);

    // -----------------------------------------------------------------------
    // Listeners & Notifications
    // -----------------------------------------------------------------------
    void addListener(StateListener listener);

private:
    std::string serializeStateLocked() const;
    void notifyListenersLocked();
    void touchAndPublishLocked(std::string& outJson);

    mutable std::mutex mutex_;
    std::filesystem::path stateFilePath_;
    std::filesystem::path stateDir_;
    protocol::HeadphoneState state_;
    std::vector<StateListener> listeners_;
    bool cleanedUp_{false};
};

} // namespace omarchy::sony

namespace omarchy::sony::daemon {
    using StateEngine = omarchy::sony::StateEngine;
    using StateListener = omarchy::sony::StateListener;
}
