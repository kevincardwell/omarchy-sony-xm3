#include "MDRProtocolV1.hpp"
#include <algorithm>
#include <sstream>
#include <chrono>

namespace omarchy::sony::protocol {

namespace {

constexpr uint8_t cmd(Command c) { return static_cast<uint8_t>(c); }

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string jsonArray(const std::array<int, 5>& v) {
    std::ostringstream ss;
    ss << "[" << v[0] << "," << v[1] << "," << v[2] << "," << v[3] << "," << v[4] << "]";
    return ss.str();
}

} // namespace

// ---------------------------------------------------------------------------
// HeadphoneState Implementation
// ---------------------------------------------------------------------------

std::string HeadphoneState::toJson() const {
    if (!connected) {
        return "{\"schema_version\":1,\"connected\":false}";
    }

    const std::string bands = jsonArray(eq_custom_bands);

    std::ostringstream ss;
    ss << "{"
       << "\"schema_version\":" << schema_version << ","
       << "\"connected\":true,"
       << "\"device_name\":\"" << device_name << "\","
       << "\"battery_level\":" << battery_level << ","
       << "\"charging\":" << (battery_charging ? "true" : "false") << ","
       << "\"battery_charging\":" << (battery_charging ? "true" : "false") << ","
       << "\"noise_mode\":\"" << noise_mode << "\","
       << "\"ambient_level\":" << ambient_sound_level << ","
       << "\"ambient_sound_level\":" << ambient_sound_level << ","
       << "\"ambient_max_level\":" << ambient_max_level << ","
       << "\"voice_passthrough\":" << (voice_passthrough ? "true" : "false") << ","
       << "\"eq_preset\":\"" << eq_preset << "\","
       << "\"eq_bands\":" << bands << ","
       << "\"eq_custom_bands\":" << bands << ","
       << "\"clear_bass\":" << clear_bass << ","
       << "\"dsee\":" << (dsee_hx ? "true" : "false") << ","
       << "\"dsee_hx\":" << (dsee_hx ? "true" : "false") << ","
       << "\"ear_detection\":" << (ear_detection ? "true" : "false") << ","
       << "\"surround\":\"" << surround << "\","
       << "\"sound_position\":\"" << sound_position << "\","
       << "\"auto_power_off\":\"" << auto_power_off << "\","
       << "\"connection_mode\":\"" << connection_mode << "\","
       << "\"codec\":\"" << codec << "\","
       << "\"last_updated\":" << last_updated
       << "}";
    return ss.str();
}

HeadphoneState HeadphoneState::makeDisconnected() {
    HeadphoneState s;
    s.schema_version = 1;
    s.connected = false;
    return s;
}

// ---------------------------------------------------------------------------
// Low-Level Framing, Escaping & Checksums
// ---------------------------------------------------------------------------

uint8_t calculateChecksum(std::span<const uint8_t> data) noexcept {
    uint8_t sum = 0;
    for (uint8_t b : data) {
        sum += b;
    }
    return sum;
}

std::vector<uint8_t> escapeBytes(std::span<const uint8_t> unescaped) {
    std::vector<uint8_t> out;
    out.reserve(unescaped.size() * 2);
    for (uint8_t b : unescaped) {
        switch (b) {
            case kEndMarker:    out.push_back(kEscapeSentry); out.push_back(kEscaped3C); break;
            case kEscapeSentry: out.push_back(kEscapeSentry); out.push_back(kEscaped3D); break;
            case kStartMarker:  out.push_back(kEscapeSentry); out.push_back(kEscaped3E); break;
            default:            out.push_back(b); break;
        }
    }
    return out;
}

std::vector<uint8_t> unescapeBytes(std::span<const uint8_t> escaped) {
    std::vector<uint8_t> out;
    out.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); ++i) {
        uint8_t b = escaped[i];
        if (b == kEscapeSentry) {
            if (i + 1 >= escaped.size()) return {}; // Incomplete escape at EOF
            uint8_t next = escaped[++i];
            switch (next) {
                case kEscaped3C: out.push_back(kEndMarker); break;
                case kEscaped3D: out.push_back(kEscapeSentry); break;
                case kEscaped3E: out.push_back(kStartMarker); break;
                default: return {}; // Invalid escape sequence
            }
        } else {
            out.push_back(b);
        }
    }
    return out;
}

std::vector<uint8_t> packFrame(PacketType type, uint8_t seq, std::span<const uint8_t> payload) {
    std::vector<uint8_t> unescaped;
    unescaped.reserve(6 + payload.size() + 1);

    unescaped.push_back(static_cast<uint8_t>(type));
    unescaped.push_back(seq);

    // 4-byte Big-Endian Length
    uint32_t len = static_cast<uint32_t>(payload.size());
    unescaped.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>(len & 0xFF));

    // Payload
    unescaped.insert(unescaped.end(), payload.begin(), payload.end());

    // 8-bit additive Checksum (modulo 256 sum over all unescaped bytes before checksum)
    uint8_t csum = calculateChecksum(unescaped);
    unescaped.push_back(csum);

    // Escape and encapsulate with start/end markers
    std::vector<uint8_t> frame;
    frame.reserve(unescaped.size() * 2 + 2);
    frame.push_back(kStartMarker);
    std::vector<uint8_t> escaped = escapeBytes(unescaped);
    frame.insert(frame.end(), escaped.begin(), escaped.end());
    frame.push_back(kEndMarker);

    return frame;
}

std::optional<UnpackedFrame> unpackFrame(std::span<const uint8_t> frameBytes) {
    if (frameBytes.size() < 9) return std::nullopt;
    if (frameBytes.front() != kStartMarker || frameBytes.back() != kEndMarker) return std::nullopt;

    // Strip markers
    std::span<const uint8_t> inner = frameBytes.subspan(1, frameBytes.size() - 2);
    std::vector<uint8_t> unescaped = unescapeBytes(inner);
    if (unescaped.size() < 7) return std::nullopt; // Type(1) + Seq(1) + Len(4) + Csum(1)

    // Verify Checksum
    uint8_t receivedCsum = unescaped.back();
    std::span<const uint8_t> checkSpan(unescaped.data(), unescaped.size() - 1);
    if (calculateChecksum(checkSpan) != receivedCsum) return std::nullopt;

    PacketType type = static_cast<PacketType>(unescaped[0]);
    uint8_t seq = unescaped[1];
    uint32_t len = (static_cast<uint32_t>(unescaped[2]) << 24) |
                   (static_cast<uint32_t>(unescaped[3]) << 16) |
                   (static_cast<uint32_t>(unescaped[4]) << 8)  |
                   static_cast<uint32_t>(unescaped[5]);

    if (unescaped.size() - 7 != len) return std::nullopt;

    std::vector<uint8_t> payload(unescaped.begin() + 6, unescaped.begin() + 6 + len);
    return UnpackedFrame{type, seq, std::move(payload)};
}

// ---------------------------------------------------------------------------
// StreamFramer Implementation
// ---------------------------------------------------------------------------

void StreamFramer::append(std::span<const uint8_t> incoming) {
    constexpr size_t kMaxBufferSize = 64 * 1024; // 64 KB
    if (buffer_.size() + incoming.size() > kMaxBufferSize) {
        buffer_.clear(); // Discard corrupted unclosed stream data to prevent unbounded growth
    }
    buffer_.insert(buffer_.end(), incoming.begin(), incoming.end());
}

std::optional<std::vector<uint8_t>> StreamFramer::nextFrame() {
    auto startIt = std::find(buffer_.begin(), buffer_.end(), kStartMarker);
    if (startIt == buffer_.end()) {
        buffer_.clear();
        return std::nullopt;
    }
    if (startIt != buffer_.begin()) {
        buffer_.erase(buffer_.begin(), startIt);
        startIt = buffer_.begin();
    }

    auto endIt = std::find(startIt + 1, buffer_.end(), kEndMarker);
    if (endIt == buffer_.end()) {
        return std::nullopt; // Incomplete frame
    }

    std::vector<uint8_t> frame(startIt, endIt + 1);
    buffer_.erase(buffer_.begin(), endIt + 1);
    return frame;
}

void StreamFramer::reset() {
    buffer_.clear();
}

// ---------------------------------------------------------------------------
// Noise-control step helpers
// ---------------------------------------------------------------------------

std::string stepToNoiseMode(uint8_t step) {
    if (step == kStepNoiseCancel)   return "anc";
    if (step == kStepWindReduction) return "wind";
    return "ambient";
}

uint8_t noiseModeToStep(NoiseMode mode, int preferredLevel, int maxStep) {
    switch (mode) {
        case NoiseMode::ANC:  return kStepNoiseCancel;
        case NoiseMode::WIND: return kStepWindReduction;
        case NoiseMode::AMBIENT: {
            const int hi = std::max<int>(kMinAmbientStep, maxStep);
            if (preferredLevel >= kMinAmbientStep && preferredLevel <= hi) {
                return static_cast<uint8_t>(preferredLevel);
            }
            // Halfway up the ambient range is a sane default when the caller
            // has no remembered level (e.g. first switch after a cold start).
            return static_cast<uint8_t>((kMinAmbientStep + hi) / 2);
        }
        case NoiseMode::OFF:
        default:
            return kStepNoiseCancel;
    }
}

// ---------------------------------------------------------------------------
// Command Serializers (Host -> XM3)
// ---------------------------------------------------------------------------

std::vector<uint8_t> serializeACK(uint8_t rx_seq) {
    return packFrame(PacketType::ACK, static_cast<uint8_t>(1 - rx_seq), {});
}

std::vector<uint8_t> serializeNcAsm(bool enabled, uint8_t step, bool voiceFocus, uint8_t seq) {
    // NCASM_SET_PARAM / NOISE_CANCELLING_AND_AMBIENT_SOUND_MODE, 8 bytes:
    //   [0] 0x68 command
    //   [1] 0x02 inquired type
    //   [2] ncAsmEffect      OFF when disabled, ADJUST_COMPLETE when enabled
    //   [3] ncSettingType    LEVEL_ADJUSTMENT
    //   [4] ncDualSingleValue  DUAL = full NC, SINGLE = wind reduction, OFF = ambient
    //   [5] asmSettingType   LEVEL_ADJUSTMENT
    //   [6] asmId            NORMAL or VOICE (Focus on Voice)
    //   [7] asmLevel         wire step, or 0xFF when disabled
    NcDualSingleValue ncValue = NcDualSingleValue::OFF;
    if (enabled) {
        if (step == kStepNoiseCancel)        ncValue = NcDualSingleValue::DUAL;
        else if (step == kStepWindReduction) ncValue = NcDualSingleValue::SINGLE;
    }

    // The headset rejects Focus on Voice outside the true ambient range.
    const bool focus = enabled && voiceFocus && step >= kMinVoiceFocusStep;

    std::vector<uint8_t> payload = {
        cmd(Command::NCASM_SET_PARAM),
        static_cast<uint8_t>(NcAsmInquiredType::NC_AND_ASM),
        static_cast<uint8_t>(enabled ? NcAsmEffect::ADJUST_COMPLETE : NcAsmEffect::OFF),
        static_cast<uint8_t>(NcAsmSettingType::LEVEL_ADJUSTMENT),
        static_cast<uint8_t>(ncValue),
        static_cast<uint8_t>(AsmSettingType::LEVEL_ADJUSTMENT),
        static_cast<uint8_t>(focus ? AsmId::VOICE : AsmId::NORMAL),
        enabled ? step : kAsmLevelDisabled
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeNoiseMode(NoiseMode mode, uint8_t ambientLevel, bool voiceFocus, uint8_t seq) {
    if (mode == NoiseMode::OFF) {
        return serializeNcAsm(false, kStepNoiseCancel, false, seq);
    }
    const uint8_t step = noiseModeToStep(mode, static_cast<int>(ambientLevel));
    return serializeNcAsm(true, step, voiceFocus, seq);
}

std::vector<uint8_t> serializeAmbientLevel(uint8_t level, bool voiceFocus, uint8_t seq) {
    const uint8_t step = static_cast<uint8_t>(std::clamp<int>(level, 0, kMaxAmbientStep));
    return serializeNcAsm(true, step, voiceFocus, seq);
}

std::vector<uint8_t> serializeEqPreset(EqPreset preset, uint8_t seq) {
    std::vector<uint8_t> payload = {
        cmd(Command::EQEBB_SET_PARAM),
        static_cast<uint8_t>(EqEbbInquiredType::PRESET_EQ),
        static_cast<uint8_t>(preset),
        0x00  // 0 band steps follow (preset selection only)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeCustomEq(const std::array<int, 5>& bands, int clearBass, uint8_t seq) {
    auto encode = [](int v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(v, -10, 10) + 10);
    };

    std::vector<uint8_t> payload = {
        cmd(Command::EQEBB_SET_PARAM),
        static_cast<uint8_t>(EqEbbInquiredType::PRESET_EQ),
        static_cast<uint8_t>(EqPreset::CUSTOM), // 0xA0 ("Manual" in the Sony app)
        0x06, // 6 band steps follow: Clear Bass then the five bands
        encode(clearBass),
        encode(bands[0]),
        encode(bands[1]),
        encode(bands[2]),
        encode(bands[3]),
        encode(bands[4])
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeDsee(bool enabled, uint8_t seq) {
    // AUDIO_SET_PARAM / UPSCALING. On the XM3 this is DSEE HX, and the setting
    // value is AUTO (0x01) or OFF (0x00) — not a plain boolean elsewhere.
    std::vector<uint8_t> payload = {
        cmd(Command::AUDIO_SET_PARAM),
        static_cast<uint8_t>(AudioInquiredType::UPSCALING),
        0x00, // UpscalingSettingType::AUTO_OFF
        static_cast<uint8_t>(enabled ? 0x01 : 0x00)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeEarDetection(bool enabled, uint8_t seq) {
    // SYSTEM_SET_PARAM / CONTROL_BY_WEARING. ON = 0x01 here (the v2 table
    // inverts this, which is a common porting mistake).
    std::vector<uint8_t> payload = {
        cmd(Command::SYSTEM_SET_PARAM),
        static_cast<uint8_t>(SystemInquiredType::CONTROL_BY_WEARING),
        0x00, // ControlByWearingSettingType::ON_OFF
        static_cast<uint8_t>(enabled ? 0x01 : 0x00)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeSurround(SurroundPreset preset, uint8_t seq) {
    std::vector<uint8_t> payload = {
        cmd(Command::VPT_SET_PARAM),
        static_cast<uint8_t>(VptInquiredType::VPT),
        static_cast<uint8_t>(preset)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeSoundPosition(SoundPosition position, uint8_t seq) {
    std::vector<uint8_t> payload = {
        cmd(Command::VPT_SET_PARAM),
        static_cast<uint8_t>(VptInquiredType::SOUND_POSITION),
        static_cast<uint8_t>(position)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeAutoPowerOff(AutoPowerOff timer, uint8_t seq) {
    // The headset takes both the "currently active" and "last selected" element
    // ids. Selecting DISABLED must keep a real timer in the select slot so the
    // Sony app still shows the previous choice; 180 min is Sony's own default.
    const uint8_t active = static_cast<uint8_t>(timer);
    const uint8_t selected = (timer == AutoPowerOff::DISABLED || timer == AutoPowerOff::WHEN_REMOVED)
        ? static_cast<uint8_t>(AutoPowerOff::AFTER_180_MIN)
        : active;

    std::vector<uint8_t> payload = {
        cmd(Command::SYSTEM_SET_PARAM),
        static_cast<uint8_t>(SystemInquiredType::AUTO_POWER_OFF),
        0x01, // AutoPowerOffParameterType::ACTIVE_AND_SELECT_TIME_ID
        active,
        selected
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeConnectionMode(ConnectionMode mode, uint8_t seq) {
    std::vector<uint8_t> payload = {
        cmd(Command::AUDIO_SET_PARAM),
        static_cast<uint8_t>(AudioInquiredType::CONNECTION_MODE),
        0x00, // ConnectionModeSettingType::SOUND_CONNECTION
        static_cast<uint8_t>(mode)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

// ---------------------------------------------------------------------------
// Query Serializers
// ---------------------------------------------------------------------------

namespace {
std::vector<uint8_t> query2(Command c, uint8_t type, uint8_t seq) {
    std::vector<uint8_t> payload = { cmd(c), type };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}
} // namespace

std::vector<uint8_t> serializeQueryBattery(uint8_t seq) {
    return query2(Command::COMMON_GET_BATTERY_LEVEL,
                  static_cast<uint8_t>(BatteryInquiredType::BATTERY), seq);
}

std::vector<uint8_t> serializeQueryNoiseMode(uint8_t seq) {
    return query2(Command::NCASM_GET_PARAM,
                  static_cast<uint8_t>(NcAsmInquiredType::NC_AND_ASM), seq);
}

std::vector<uint8_t> serializeQueryNcAsmCapability(uint8_t seq) {
    return query2(Command::NCASM_GET_CAPABILITY,
                  static_cast<uint8_t>(NcAsmInquiredType::NC_AND_ASM), seq);
}

std::vector<uint8_t> serializeQueryEq(uint8_t seq) {
    return query2(Command::EQEBB_GET_PARAM,
                  static_cast<uint8_t>(EqEbbInquiredType::PRESET_EQ), seq);
}

std::vector<uint8_t> serializeQueryDsee(uint8_t seq) {
    return query2(Command::AUDIO_GET_PARAM,
                  static_cast<uint8_t>(AudioInquiredType::UPSCALING), seq);
}

std::vector<uint8_t> serializeQueryUpscalingEffect(uint8_t seq) {
    return query2(Command::COMMON_GET_UPSCALING_EFFECT,
                  static_cast<uint8_t>(CommonInquiredType::FIXED_VALUE), seq);
}

std::vector<uint8_t> serializeQueryCodec(uint8_t seq) {
    return query2(Command::COMMON_GET_AUDIO_CODEC,
                  static_cast<uint8_t>(CommonInquiredType::FIXED_VALUE), seq);
}

std::vector<uint8_t> serializeQueryEarDetection(uint8_t seq) {
    return query2(Command::SYSTEM_GET_PARAM,
                  static_cast<uint8_t>(SystemInquiredType::CONTROL_BY_WEARING), seq);
}

std::vector<uint8_t> serializeQuerySurround(uint8_t seq) {
    return query2(Command::VPT_GET_PARAM,
                  static_cast<uint8_t>(VptInquiredType::VPT), seq);
}

std::vector<uint8_t> serializeQuerySoundPosition(uint8_t seq) {
    return query2(Command::VPT_GET_PARAM,
                  static_cast<uint8_t>(VptInquiredType::SOUND_POSITION), seq);
}

std::vector<uint8_t> serializeQueryAutoPowerOff(uint8_t seq) {
    return query2(Command::SYSTEM_GET_PARAM,
                  static_cast<uint8_t>(SystemInquiredType::AUTO_POWER_OFF), seq);
}

std::vector<uint8_t> serializeQueryConnectionMode(uint8_t seq) {
    return query2(Command::AUDIO_GET_PARAM,
                  static_cast<uint8_t>(AudioInquiredType::CONNECTION_MODE), seq);
}

// ---------------------------------------------------------------------------
// Inbound State Deserializer (XM3 -> Host)
// ---------------------------------------------------------------------------

bool parseInboundPayload(std::span<const uint8_t> payload, HeadphoneState& state) {
    if (payload.empty()) return false;
    const uint8_t c = payload[0];
    bool updated = false;

    // 1. Battery level (COMMON_RET/NTFY_BATTERY_LEVEL)
    if ((c == cmd(Command::COMMON_RET_BATTERY_LEVEL) ||
         c == cmd(Command::COMMON_NTFY_BATTERY_LEVEL)) && payload.size() >= 4) {
        if (payload[1] == static_cast<uint8_t>(BatteryInquiredType::BATTERY)) {
            state.battery_level = payload[2];
            state.battery_charging = (payload[3] == 0x01);
            updated = true;
        }
    }
    // 2. Audio codec (COMMON_RET/NTFY_AUDIO_CODEC)
    else if ((c == cmd(Command::COMMON_RET_AUDIO_CODEC) ||
              c == cmd(Command::COMMON_NTFY_AUDIO_CODEC)) && payload.size() >= 3) {
        state.codec = codecToString(payload[2]);
        updated = true;
    }
    // 3. DSEE HX availability report (COMMON_RET/NTFY_UPSCALING_EFFECT)
    //    payload: [cmd, 0x00, effectType, effectStatus] where status VALID = active
    else if ((c == cmd(Command::COMMON_RET_UPSCALING_EFFECT) ||
              c == cmd(Command::COMMON_NTFY_UPSCALING_EFFECT)) && payload.size() >= 4) {
        state.dsee_hx = (payload[3] == 0x01);
        updated = true;
    }
    // 4. Noise control (NCASM_RET/NTFY_PARAM)
    else if ((c == cmd(Command::NCASM_RET_PARAM) ||
              c == cmd(Command::NCASM_NTFY_PARAM)) && payload.size() >= 8) {
        if (payload[1] == static_cast<uint8_t>(NcAsmInquiredType::NC_AND_ASM)) {
            const uint8_t effect  = payload[2];
            const uint8_t ncValue = payload[4];
            const uint8_t asmId   = payload[6];
            const uint8_t level   = payload[7];

            if (effect == static_cast<uint8_t>(NcAsmEffect::OFF)) {
                state.noise_mode = "off";
            } else if (ncValue == static_cast<uint8_t>(NcDualSingleValue::DUAL)) {
                state.noise_mode = "anc";
                state.ambient_sound_level = kStepNoiseCancel;
            } else if (ncValue == static_cast<uint8_t>(NcDualSingleValue::SINGLE)) {
                state.noise_mode = "wind";
                state.ambient_sound_level = kStepWindReduction;
            } else {
                state.noise_mode = "ambient";
                if (level != kAsmLevelDisabled) {
                    state.ambient_sound_level = level;
                }
            }
            state.voice_passthrough = (asmId == static_cast<uint8_t>(AsmId::VOICE));
            updated = true;
        }
    }
    // 5. Noise control capability (NCASM_RET_CAPABILITY)
    //    payload: [0x61, 0x02, ncSettingType, ncStep, asmSettingType, count, (asmId, steps)...]
    else if (c == cmd(Command::NCASM_RET_CAPABILITY) && payload.size() >= 6) {
        if (payload[1] == static_cast<uint8_t>(NcAsmInquiredType::NC_AND_ASM)) {
            const size_t count = payload[5];
            for (size_t i = 0; i < count; ++i) {
                const size_t base = 6 + i * 2;
                if (base + 1 >= payload.size()) break;
                if (payload[base] == static_cast<uint8_t>(AsmId::NORMAL) && payload[base + 1] > 0) {
                    // The headset reports a step *count*; the top usable step is one less.
                    state.ambient_max_level = static_cast<int>(payload[base + 1]) - 1;
                    updated = true;
                }
            }
        }
    }
    // 6. Equalizer (EQEBB_RET/NTFY_PARAM)
    else if ((c == cmd(Command::EQEBB_RET_PARAM) ||
              c == cmd(Command::EQEBB_NTFY_PARAM)) && payload.size() >= 3) {
        if (payload[1] == static_cast<uint8_t>(EqEbbInquiredType::PRESET_EQ)) {
            state.eq_preset = eqPresetToString(static_cast<EqPreset>(payload[2]));
            // Band steps are [Clear Bass, band0..band4], each biased by +10.
            // A corrupt byte would otherwise decode to something like +245, so
            // clamp here rather than leaking it into status.json.
            if (payload.size() >= 10 && payload[3] == 0x06) {
                auto decode = [](uint8_t raw) {
                    return std::clamp(static_cast<int>(raw) - 10, -10, 10);
                };
                state.clear_bass = decode(payload[4]);
                for (size_t i = 0; i < 5; ++i) {
                    state.eq_custom_bands[i] = decode(payload[5 + i]);
                }
            }
            updated = true;
        }
    }
    // 7. VPT: surround and sound position (VPT_RET/NTFY_PARAM)
    else if ((c == cmd(Command::VPT_RET_PARAM) ||
              c == cmd(Command::VPT_NTFY_PARAM)) && payload.size() >= 3) {
        if (payload[1] == static_cast<uint8_t>(VptInquiredType::VPT)) {
            state.surround = surroundToString(static_cast<SurroundPreset>(payload[2]));
            updated = true;
        } else if (payload[1] == static_cast<uint8_t>(VptInquiredType::SOUND_POSITION)) {
            state.sound_position = soundPositionToString(static_cast<SoundPosition>(payload[2]));
            updated = true;
        }
    }
    // 8. Audio params: DSEE HX setting and connection mode (AUDIO_RET/NTFY_PARAM)
    else if ((c == cmd(Command::AUDIO_RET_PARAM) ||
              c == cmd(Command::AUDIO_NTFY_PARAM)) && payload.size() >= 4) {
        if (payload[1] == static_cast<uint8_t>(AudioInquiredType::UPSCALING)) {
            state.dsee_hx = (payload[3] == 0x01);
            updated = true;
        } else if (payload[1] == static_cast<uint8_t>(AudioInquiredType::CONNECTION_MODE)) {
            state.connection_mode = connectionModeToString(static_cast<ConnectionMode>(payload[3]));
            updated = true;
        }
    }
    // 9. System params: wearing detection and auto power off (SYSTEM_RET/NTFY_PARAM)
    else if ((c == cmd(Command::SYSTEM_RET_PARAM) ||
              c == cmd(Command::SYSTEM_NTFY_PARAM)) && payload.size() >= 4) {
        if (payload[1] == static_cast<uint8_t>(SystemInquiredType::CONTROL_BY_WEARING)) {
            state.ear_detection = (payload[3] == 0x01);
            updated = true;
        } else if (payload[1] == static_cast<uint8_t>(SystemInquiredType::AUTO_POWER_OFF)) {
            state.auto_power_off = autoPowerOffToString(static_cast<AutoPowerOff>(payload[3]));
            updated = true;
        }
    }

    if (updated) {
        state.connected = true;
        state.last_updated = nowSeconds();
    }
    return updated;
}

// ---------------------------------------------------------------------------
// Enum <-> String Helpers
// ---------------------------------------------------------------------------

std::string eqPresetToString(EqPreset preset) {
    switch (preset) {
        case EqPreset::OFF:     return "off";
        case EqPreset::BRIGHT:  return "bright";
        case EqPreset::EXCITED: return "excited";
        case EqPreset::MELLOW:  return "mellow";
        case EqPreset::RELAXED: return "relaxed";
        case EqPreset::VOCAL:   return "vocal";
        case EqPreset::TREBLE:  return "treble";
        case EqPreset::BASS:    return "bass";
        case EqPreset::SPEECH:  return "speech";
        case EqPreset::CUSTOM:  return "custom";
        case EqPreset::USER1:   return "user1";
        case EqPreset::USER2:   return "user2";
        default:                return "off";
    }
}

EqPreset stringToEqPreset(const std::string& str) {
    if (str == "off")     return EqPreset::OFF;
    if (str == "bright")  return EqPreset::BRIGHT;
    if (str == "excited") return EqPreset::EXCITED;
    if (str == "mellow")  return EqPreset::MELLOW;
    if (str == "relaxed") return EqPreset::RELAXED;
    if (str == "vocal")   return EqPreset::VOCAL;
    if (str == "treble")  return EqPreset::TREBLE;
    if (str == "bass")    return EqPreset::BASS;
    if (str == "speech")  return EqPreset::SPEECH;
    if (str == "custom" || str == "manual") return EqPreset::CUSTOM;
    if (str == "user1")   return EqPreset::USER1;
    if (str == "user2")   return EqPreset::USER2;
    return EqPreset::OFF;
}

std::string noiseModeToString(NoiseMode mode) {
    switch (mode) {
        case NoiseMode::OFF:     return "off";
        case NoiseMode::ANC:     return "anc";
        case NoiseMode::AMBIENT: return "ambient";
        case NoiseMode::WIND:    return "wind";
        default:                 return "off";
    }
}

NoiseMode stringToNoiseMode(const std::string& str) {
    if (str == "anc")     return NoiseMode::ANC;
    if (str == "ambient") return NoiseMode::AMBIENT;
    if (str == "wind")    return NoiseMode::WIND;
    if (str == "off")     return NoiseMode::OFF;
    return NoiseMode::ANC;
}

std::string surroundToString(SurroundPreset preset) {
    switch (preset) {
        case SurroundPreset::OFF:              return "off";
        case SurroundPreset::OUTDOOR_FESTIVAL: return "outdoor";
        case SurroundPreset::ARENA:            return "arena";
        case SurroundPreset::CONCERT_HALL:     return "concert";
        case SurroundPreset::CLUB:             return "club";
        default:                               return "off";
    }
}

SurroundPreset stringToSurround(const std::string& str) {
    if (str == "off")     return SurroundPreset::OFF;
    if (str == "outdoor") return SurroundPreset::OUTDOOR_FESTIVAL;
    if (str == "arena")   return SurroundPreset::ARENA;
    if (str == "concert") return SurroundPreset::CONCERT_HALL;
    if (str == "club")    return SurroundPreset::CLUB;
    return SurroundPreset::UNKNOWN;
}

std::string soundPositionToString(SoundPosition position) {
    switch (position) {
        case SoundPosition::OFF:         return "off";
        case SoundPosition::FRONT_LEFT:  return "front-left";
        case SoundPosition::FRONT_RIGHT: return "front-right";
        case SoundPosition::FRONT:       return "front";
        case SoundPosition::REAR_LEFT:   return "rear-left";
        case SoundPosition::REAR_RIGHT:  return "rear-right";
        default:                         return "off";
    }
}

SoundPosition stringToSoundPosition(const std::string& str) {
    if (str == "off")         return SoundPosition::OFF;
    if (str == "front-left")  return SoundPosition::FRONT_LEFT;
    if (str == "front-right") return SoundPosition::FRONT_RIGHT;
    if (str == "front")       return SoundPosition::FRONT;
    if (str == "rear-left")   return SoundPosition::REAR_LEFT;
    if (str == "rear-right")  return SoundPosition::REAR_RIGHT;
    return SoundPosition::UNKNOWN;
}

std::string autoPowerOffToString(AutoPowerOff timer) {
    switch (timer) {
        case AutoPowerOff::AFTER_5_MIN:   return "5min";
        case AutoPowerOff::AFTER_30_MIN:  return "30min";
        case AutoPowerOff::AFTER_60_MIN:  return "60min";
        case AutoPowerOff::AFTER_180_MIN: return "180min";
        case AutoPowerOff::WHEN_REMOVED:  return "on-remove";
        case AutoPowerOff::DISABLED:      return "off";
        default:                          return "unknown";
    }
}

AutoPowerOff stringToAutoPowerOff(const std::string& str) {
    if (str == "5min")      return AutoPowerOff::AFTER_5_MIN;
    if (str == "30min")     return AutoPowerOff::AFTER_30_MIN;
    if (str == "60min")     return AutoPowerOff::AFTER_60_MIN;
    if (str == "180min")    return AutoPowerOff::AFTER_180_MIN;
    if (str == "on-remove") return AutoPowerOff::WHEN_REMOVED;
    if (str == "off")       return AutoPowerOff::DISABLED;
    return AutoPowerOff::UNKNOWN;
}

std::string connectionModeToString(ConnectionMode mode) {
    switch (mode) {
        case ConnectionMode::SOUND_QUALITY: return "quality";
        case ConnectionMode::STABLE_LINK:   return "stable";
        default:                            return "unknown";
    }
}

ConnectionMode stringToConnectionMode(const std::string& str) {
    if (str == "quality") return ConnectionMode::SOUND_QUALITY;
    if (str == "stable")  return ConnectionMode::STABLE_LINK;
    return ConnectionMode::UNKNOWN;
}

std::string codecToString(uint8_t codecByte) {
    switch (codecByte) {
        case 0x01: return "SBC";
        case 0x02: return "AAC";
        case 0x10: return "LDAC";
        case 0x20: return "aptX";
        case 0x21: return "aptX HD";
        case 0x00: return "";      // UNSETTLED
        default:   return "Other";
    }
}

} // namespace omarchy::sony::protocol
