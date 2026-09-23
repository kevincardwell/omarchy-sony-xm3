#pragma once

#include "MDRProtocolV1.hpp"
#include <string>
#include <functional>
#include <array>

namespace omarchy::sony {

// The daemon's view of the headset, held in memory only. Clients read it over
// the socket (`status`, or `subscribe` for every change), so nothing is
// written to disk.
class StateEngine {
public:
    StateEngine();

    [[nodiscard]] const protocol::HeadphoneState& getState() const noexcept { return state_; }
    // For the inbound parser, which writes the fields a reply carries.
    [[nodiscard]] protocol::HeadphoneState& state() noexcept { return state_; }
    [[nodiscard]] std::string getStatusJson() const { return state_.toJson(); }

    void setConnected(bool connected, const std::string& deviceName = "");
    void setBattery(int level, bool charging);

    // Setters that take a name validate it and refuse anything unknown.
    bool setNoiseMode(const std::string& mode);
    bool updateNoiseMode(const std::string& mode, int ambientLevel, bool voiceFocus);
    bool setAmbientLevel(int level);
    void setAmbientMaxLevel(int maxLevel) { state_.ambient_max_level = maxLevel; }
    void setVoicePassthrough(bool passthrough) { state_.voice_passthrough = passthrough; }
    bool setEqPreset(const std::string& preset);
    // slot: "custom" (Manual), "user1" or "user2"
    bool setCustomEq(const std::array<int, 5>& bands, int clearBass, const std::string& slot);
    void setDsee(bool enabled) { state_.dsee_hx = enabled; }
    bool setSurround(const std::string& preset);
    bool setSoundPosition(const std::string& position);
    bool setAutoPowerOff(const std::string& timer);
    bool setConnectionMode(const std::string& mode);
    void setCodec(const std::string& codec) { state_.codec = codec; }

    void modifyState(const std::function<void(protocol::HeadphoneState&)>& mutator) { mutator(state_); }

private:
    protocol::HeadphoneState state_;
};

// Low-battery warnings, at 20% and again at 10%. Returns the threshold to warn
// about now, or 0. `warned` (start it at 0) remembers the lowest threshold
// already announced, so a reconnect at the same level stays quiet; charging,
// or the level rising back above a threshold, re-arms it.
int batteryWarning(int level, bool charging, int& warned);

} // namespace omarchy::sony
