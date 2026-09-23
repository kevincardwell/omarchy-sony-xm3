#include "StateEngine.hpp"

namespace omarchy::sony {

StateEngine::StateEngine() {
    // Placeholder state until the headset answers. Unknown values stay unknown:
    // a plausible-looking default (it used to claim LDAC) is worse than blank.
    state_.connected = false;
    state_.ambient_sound_level = protocol::kStepNoiseCancel;
}

void StateEngine::setConnected(bool connected, const std::string& deviceName) {
    state_.connected = connected;
    if (connected && !deviceName.empty()) {
        state_.device_name = deviceName;
    }
}

void StateEngine::setBattery(int level, bool charging) {
    state_.battery_level = level;
    state_.battery_charging = charging;
}

bool StateEngine::setNoiseMode(const std::string& mode) {
    if (mode != "anc" && mode != "ambient" && mode != "wind" && mode != "off") {
        return false;
    }
    state_.noise_mode = mode;
    if (mode == "anc")  state_.ambient_sound_level = protocol::kStepNoiseCancel;
    if (mode == "wind") state_.ambient_sound_level = protocol::kStepWindReduction;
    return true;
}

bool StateEngine::updateNoiseMode(const std::string& mode, int ambientLevel, bool voiceFocus) {
    if (!setNoiseMode(mode)) {
        return false;
    }
    if (mode == "ambient") {
        state_.ambient_sound_level = ambientLevel;
    }
    state_.voice_passthrough = voiceFocus;
    return true;
}

bool StateEngine::setAmbientLevel(int level) {
    if (level < 0 || level > state_.ambient_max_level) {
        return false;
    }
    state_.ambient_sound_level = level;
    // The level *is* the mode on this generation of headset, so keep the
    // reported mode consistent with the step that was just applied.
    state_.noise_mode = protocol::stepToNoiseMode(static_cast<uint8_t>(level));
    return true;
}

bool StateEngine::setEqPreset(const std::string& preset) {
    static const char* kPresets[] = {
        "off", "bright", "excited", "mellow", "relaxed", "vocal",
        "treble", "bass", "speech", "custom", "user1", "user2"
    };
    for (const char* p : kPresets) {
        if (preset == p) {
            state_.eq_preset = preset;
            return true;
        }
    }
    return false;
}

bool StateEngine::setCustomEq(const std::array<int, 5>& bands, int clearBass, const std::string& slot) {
    if (slot != "custom" && slot != "user1" && slot != "user2") return false;
    for (int b : bands) {
        if (b < -10 || b > 10) return false;
    }
    if (clearBass < -10 || clearBass > 10) return false;

    state_.eq_preset = slot;
    state_.eq_custom_bands = bands;
    state_.clear_bass = clearBass;
    return true;
}

bool StateEngine::setSurround(const std::string& preset) {
    if (protocol::stringToSurround(preset) == protocol::SurroundPreset::UNKNOWN) return false;
    state_.surround = preset;
    return true;
}

bool StateEngine::setSoundPosition(const std::string& position) {
    if (protocol::stringToSoundPosition(position) == protocol::SoundPosition::UNKNOWN) return false;
    state_.sound_position = position;
    return true;
}

bool StateEngine::setAutoPowerOff(const std::string& timer) {
    if (protocol::stringToAutoPowerOff(timer) == protocol::AutoPowerOff::UNKNOWN) return false;
    state_.auto_power_off = timer;
    return true;
}

bool StateEngine::setConnectionMode(const std::string& mode) {
    if (protocol::stringToConnectionMode(mode) == protocol::ConnectionMode::UNKNOWN) return false;
    state_.connection_mode = mode;
    return true;
}

int batteryWarning(int level, bool charging, int& warned) {
    if (level < 0) return 0;
    if (charging || level > 20) {
        warned = 0;
        return 0;
    }
    if (level > 10 && warned == 10) warned = 20;  // back above 10: re-arm it
    const int threshold = level <= 10 ? 10 : 20;
    if (warned != 0 && warned <= threshold) return 0;
    warned = threshold;
    return threshold;
}

} // namespace omarchy::sony
