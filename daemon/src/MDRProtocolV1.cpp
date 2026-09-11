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
       << "\"dsee_hx_active\":" << (dsee_hx_active ? "true" : "false") << ","
       << "\"surround\":\"" << surround << "\","
       << "\"sound_position\":\"" << sound_position << "\","
       << "\"auto_power_off\":\"" << auto_power_off << "\","
       << "\"connection_mode\":\"" << connection_mode << "\","
       << "\"codec\":\"" << codec << "\","
       << "\"firmware_version\":\"" << firmware_version << "\","
       << "\"optimizer_state\":\"" << optimizer_state << "\","
       << "\"optimizer_pressure\":\"" << optimizer_pressure << "\","
       << "\"volume\":" << volume << ","
       << "\"volume_max\":" << volume_max << ","
       << "\"nc_button\":\"" << nc_button << "\","
       << "\"touch_panel\":" << (touch_panel ? "true" : "false") << ","
       << "\"voice_guidance\":" << (voice_guidance ? "true" : "false") << ","
       << "\"voice_guidance_language\":\"" << voice_guidance_language << "\","
       << "\"model_name\":\"" << model_name << "\","
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

std::vector<uint8_t> serializeCustomEq(const std::array<int, 5>& bands, int clearBass, uint8_t seq,
                                       EqPreset slot) {
    auto encode = [](int v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(v, -10, 10) + 10);
    };

    std::vector<uint8_t> payload = {
        cmd(Command::EQEBB_SET_PARAM),
        static_cast<uint8_t>(EqEbbInquiredType::PRESET_EQ),
        // 0xA0 is "Manual" in the Sony app; 0xA1/0xA2 are Custom 1 and 2.
        static_cast<uint8_t>(slot == EqPreset::USER1 || slot == EqPreset::USER2 ? slot : EqPreset::CUSTOM),
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

std::vector<uint8_t> serializeOptimizer(bool start, uint8_t seq) {
    // OPT_SET_STATUS / NC_OPTIMIZER / ENABLE / START(1) or CANCEL(0)
    std::vector<uint8_t> payload = {cmd(Command::OPT_SET_STATUS), 0x01, 0x00,
                                    static_cast<uint8_t>(start ? 0x01 : 0x00)};
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeVolume(uint8_t volume, uint8_t seq) {
    // PLAY_SET_PARAM / PLAYBACK_CONTROLLER / VOLUME
    std::vector<uint8_t> payload = {cmd(Command::PLAY_SET_PARAM), 0x01, 0x20, volume};
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializePlayback(PlaybackControl control, uint8_t seq) {
    // PLAY_SET_STATUS / PLAYBACK_CONTROLLER / ENABLE / control
    std::vector<uint8_t> payload = {cmd(Command::PLAY_SET_STATUS), 0x01, 0x00,
                                    static_cast<uint8_t>(control)};
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeNcButton(NcButton button, uint8_t seq) {
    std::vector<uint8_t> payload = {cmd(Command::GENERAL_SETTING_SET_PARAM), kGsNcButton,
                                    kGsTypeList, static_cast<uint8_t>(button)};
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeTouchPanel(bool enabled, uint8_t seq) {
    // Boolean general settings use ON = 0x01 on the XM3 (the XM5 inverts it).
    std::vector<uint8_t> payload = {cmd(Command::GENERAL_SETTING_SET_PARAM), kGsTouchPanel,
                                    kGsTypeBoolean, static_cast<uint8_t>(enabled ? 0x01 : 0x00)};
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeVoiceGuidance(bool enabled, uint8_t seq) {
    // Table 2: VOICE_GUIDANCE_SET_PARAM / VOICE_GUIDANCE_SETTING / ON_OFF
    std::vector<uint8_t> payload = {static_cast<uint8_t>(CommandT2::VOICE_GUIDANCE_SET_PARAM), 0x01, 0x01,
                                    static_cast<uint8_t>(enabled ? 0x01 : 0x00)};
    return packFrame(PacketType::DATA_MDR_NO2, seq, payload);
}

std::vector<uint8_t> serializeAlertReply(uint8_t messageType, bool proceed, uint8_t seq) {
    // ALERT_SET_PARAM / FIXED_MESSAGE / message / POSITIVE(1) or NEGATIVE(0)
    std::vector<uint8_t> payload = {cmd(Command::ALERT_SET_PARAM), 0x01, messageType,
                                    static_cast<uint8_t>(proceed ? 0x01 : 0x00)};
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

std::vector<uint8_t> serializeQueryProtocolInfo(uint8_t seq) {
    return query2(Command::CONNECT_GET_PROTOCOL_INFO,
                  static_cast<uint8_t>(CommonInquiredType::FIXED_VALUE), seq);
}

std::vector<uint8_t> serializeQueryCapabilityInfo(uint8_t seq) {
    return query2(Command::CONNECT_GET_CAPABILITY_INFO,
                  static_cast<uint8_t>(CommonInquiredType::FIXED_VALUE), seq);
}

std::vector<uint8_t> serializeQuerySupportFunction(uint8_t seq) {
    return query2(Command::CONNECT_GET_SUPPORT_FUNCTION,
                  static_cast<uint8_t>(CommonInquiredType::FIXED_VALUE), seq);
}

std::vector<uint8_t> serializeQueryModelName(uint8_t seq) {
    return query2(Command::CONNECT_GET_DEVICE_INFO, 0x01 /* MODEL_NAME */, seq);
}

std::vector<uint8_t> serializeQueryFirmwareVersion(uint8_t seq) {
    return query2(Command::CONNECT_GET_DEVICE_INFO, 0x02 /* FW_VERSION */, seq);
}

std::vector<uint8_t> serializeRaw(std::span<const uint8_t> payload, uint8_t seq) {
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeRawT2(std::span<const uint8_t> payload, uint8_t seq) {
    return packFrame(PacketType::DATA_MDR_NO2, seq, payload);
}

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

std::vector<uint8_t> serializeQueryOptimizerStatus(uint8_t seq) {
    return query2(Command::OPT_GET_STATUS, 0x01, seq);
}

std::vector<uint8_t> serializeQueryOptimizerParam(uint8_t seq) {
    return query2(Command::OPT_GET_PARAM, 0x01, seq);
}

std::vector<uint8_t> serializeQueryPlaybackCapability(uint8_t seq) {
    return query2(Command::PLAY_GET_CAPABILITY, 0x01, seq);
}

std::vector<uint8_t> serializeQueryVolume(uint8_t seq) {
    std::vector<uint8_t> payload = {cmd(Command::PLAY_GET_PARAM), 0x01, 0x20};
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryNcButton(uint8_t seq) {
    return query2(Command::GENERAL_SETTING_GET_PARAM, kGsNcButton, seq);
}

std::vector<uint8_t> serializeQueryTouchPanel(uint8_t seq) {
    return query2(Command::GENERAL_SETTING_GET_PARAM, kGsTouchPanel, seq);
}

std::vector<uint8_t> serializeQueryVoiceGuidance(uint8_t seq) {
    std::vector<uint8_t> payload = {static_cast<uint8_t>(CommandT2::VOICE_GUIDANCE_GET_PARAM), 0x01, 0x01};
    return packFrame(PacketType::DATA_MDR_NO2, seq, payload);
}

std::vector<uint8_t> serializeQueryVoiceGuidanceLanguage(uint8_t seq) {
    std::vector<uint8_t> payload = {static_cast<uint8_t>(CommandT2::VOICE_GUIDANCE_GET_PARAM), 0x01, 0x02};
    return packFrame(PacketType::DATA_MDR_NO2, seq, payload);
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

    // 0. Device info (CONNECT_RET_DEVICE_INFO): [0x05, type, len, ascii...]
    if (c == cmd(Command::CONNECT_RET_DEVICE_INFO) && payload.size() >= 3) {
        const size_t len = payload[2];
        if (payload.size() >= 3 + len) {
            std::string text;
            for (size_t i = 0; i < len; ++i) {
                const char ch = static_cast<char>(payload[3 + i]);
                // Printable ASCII only: this goes straight into a JSON string.
                if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\') text.push_back(ch);
            }
            if (payload[1] == 0x01) {
                state.model_name = text;
                if (!text.empty()) state.device_name = text;
                updated = true;
            }
            if (payload[1] == 0x02) { state.firmware_version = text; updated = true; }
        }
    }
    // 1. Battery level (COMMON_RET/NTFY_BATTERY_LEVEL)
    else if ((c == cmd(Command::COMMON_RET_BATTERY_LEVEL) ||
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
    // 3. DSEE HX activity (COMMON_RET/NTFY_UPSCALING_EFFECT)
    //    payload: [cmd, 0x00, effectType, effectStatus]; VALID (0x01) = processing.
    //    This is whether it is running, not whether it is switched on — the
    //    headset reports INVALID on LDAC even with the setting on — so it must
    //    never overwrite dsee_hx.
    else if ((c == cmd(Command::COMMON_RET_UPSCALING_EFFECT) ||
              c == cmd(Command::COMMON_NTFY_UPSCALING_EFFECT)) && payload.size() >= 4) {
        state.dsee_hx_active = (payload[3] == 0x01);
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
    // 9. System params: auto power off (SYSTEM_RET/NTFY_PARAM). The XM3 has
    //    no wearing sensor, so CONTROL_BY_WEARING never appears here.
    else if ((c == cmd(Command::SYSTEM_RET_PARAM) ||
              c == cmd(Command::SYSTEM_NTFY_PARAM)) && payload.size() >= 4) {
        if (payload[1] == static_cast<uint8_t>(SystemInquiredType::AUTO_POWER_OFF)) {
            state.auto_power_off = autoPowerOffToString(static_cast<AutoPowerOff>(payload[3]));
            updated = true;
        }
    }

    // 10. NC Optimizer progress (OPT_RET/NTFY_STATUS): [cmd, 0x01, status, optimizerStatus]
    else if ((c == cmd(Command::OPT_RET_STATUS) ||
              c == cmd(Command::OPT_NTFY_STATUS)) && payload.size() >= 4 && payload[1] == 0x01) {
        state.optimizer_state = optimizerStateToString(payload[3]);
        updated = true;
    }
    // 11. NC Optimizer result (OPT_RET/NTFY_PARAM):
    //     [cmd, 0x01, personalType, personalValue, barometricType, barometricValue]
    //     barometricValue 0x07..0x0A is the measured pressure, 0.7..1.0 atm.
    else if ((c == cmd(Command::OPT_RET_PARAM) ||
              c == cmd(Command::OPT_NTFY_PARAM)) && payload.size() >= 6 && payload[1] == 0x01) {
        const uint8_t baro = payload[5];
        if (payload[4] == 0x01 && baro >= 0x07 && baro <= 0x0A) {
            state.optimizer_pressure = baro == 0x0A ? "1.0" : "0." + std::to_string(baro);
        } else {
            state.optimizer_pressure = "";
        }
        updated = true;
    }
    // 12. Playback capability (PLAY_RET_CAPABILITY): [cmd, 0x01, volumeSteps, ...]
    else if (c == cmd(Command::PLAY_RET_CAPABILITY) && payload.size() >= 3 && payload[1] == 0x01) {
        if (payload[2] > 0) {
            state.volume_max = static_cast<int>(payload[2]) - 1;
            updated = true;
        }
    }
    // 13. Headset volume (PLAY_RET/NTFY_PARAM): [cmd, 0x01, 0x20 VOLUME, value]
    else if ((c == cmd(Command::PLAY_RET_PARAM) ||
              c == cmd(Command::PLAY_NTFY_PARAM)) && payload.size() >= 4 &&
             payload[1] == 0x01 && payload[2] == 0x20) {
        state.volume = std::min<int>(payload[3], state.volume_max);
        updated = true;
    }
    // 14. General settings (GENERAL_SETTING_RET/NTFY_PARAM): [cmd, slot, type, value]
    else if ((c == cmd(Command::GENERAL_SETTING_RET_PARAM) ||
              c == cmd(Command::GENERAL_SETTING_NTFY_PARAM)) && payload.size() >= 4) {
        if (payload[1] == kGsNcButton && payload[2] == kGsTypeList) {
            state.nc_button = ncButtonToString(static_cast<NcButton>(payload[3]));
            updated = true;
        } else if (payload[1] == kGsTouchPanel && payload[2] == kGsTypeBoolean) {
            state.touch_panel = (payload[3] == 0x01);
            updated = true;
        }
    }

    if (updated) {
        state.connected = true;
        state.last_updated = nowSeconds();
    }
    return updated;
}

bool parseInboundPayloadT2(std::span<const uint8_t> payload, HeadphoneState& state) {
    if (payload.size() < 4) return false;
    const uint8_t c = payload[0];
    bool updated = false;

    // Voice guidance (VOICE_GUIDANCE_RET/NTFY_PARAM): [cmd, 0x01, detail, value]
    //   detail 0x01 = on/off, 0x02 = language
    if ((c == static_cast<uint8_t>(CommandT2::VOICE_GUIDANCE_RET_PARAM) ||
         c == static_cast<uint8_t>(CommandT2::VOICE_GUIDANCE_NTFY_PARAM)) && payload[1] == 0x01) {
        if (payload[2] == 0x01) {
            state.voice_guidance = (payload[3] == 0x01);
            updated = true;
        } else if (payload[2] == 0x02) {
            state.voice_guidance_language = voiceGuidanceLanguageToString(payload[3]);
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
    // "When removed" is deliberately not selectable: it needs the wearing
    // sensor the XM3 lacks, and the headset ignores it (observed on hardware).
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

std::string ncButtonToString(NcButton button) {
    switch (button) {
        case NcButton::AMBIENT_SOUND_CONTROL: return "ambient";
        case NcButton::GOOGLE_ASSISTANT:      return "google-assistant";
        case NcButton::AMAZON_ALEXA:          return "alexa";
        default:                              return "unknown";
    }
}

NcButton stringToNcButton(const std::string& str) {
    if (str == "ambient")          return NcButton::AMBIENT_SOUND_CONTROL;
    if (str == "google-assistant") return NcButton::GOOGLE_ASSISTANT;
    if (str == "alexa")            return NcButton::AMAZON_ALEXA;
    return NcButton::UNKNOWN;
}

PlaybackControl stringToPlayback(const std::string& str) {
    if (str == "play")     return PlaybackControl::PLAY;
    if (str == "pause")    return PlaybackControl::PAUSE;
    if (str == "next")     return PlaybackControl::NEXT;
    if (str == "previous") return PlaybackControl::PREVIOUS;
    return PlaybackControl::UNKNOWN;
}

std::string optimizerStateToString(uint8_t status) {
    switch (status) {
        case 0x00: return "idle";
        case 0x01: return "measuring-fit";
        case 0x02: return "measuring-pressure";
        case 0x10: return "optimizing";
        case 0x11: return "done";
        default:   return "idle";
    }
}

std::string voiceGuidanceLanguageToString(uint8_t lang) {
    static const char* names[] = {
        "", "English", "French", "German", "Spanish", "Italian", "Portuguese", "Dutch",
        "Swedish", "Finnish", "Russian", "Japanese", "Brazilian Portuguese", "Korean",
        "Turkish", "Chinese"
    };
    return lang < sizeof(names) / sizeof(names[0]) ? names[lang] : "";
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
