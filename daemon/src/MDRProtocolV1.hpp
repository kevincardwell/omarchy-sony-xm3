#pragma once

// ---------------------------------------------------------------------------
// Sony MDR protocol, version 1 / message table 1 ("v1 T1").
//
// This is the command table spoken by the WH-1000XM3 (and its generation of
// Sony headsets). It is NOT the table used by the XM5, which speaks "v2" with
// a different set of inquired-type bytes for the same nominal features.
//
// Frame layout is shared between v1 and v2:
//   <0x3E> ESCAPE( <type> <seq> <BE32 payload length> <payload> <checksum> ) <0x3C>
//
// See docs/protocol-v1.md for the full derivation and byte-level tables.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string>
#include <array>
#include <optional>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// Wire Framing Constants
// ---------------------------------------------------------------------------
inline constexpr uint8_t kStartMarker     = 0x3E; // '>' (Start delimiter)
inline constexpr uint8_t kEndMarker       = 0x3C; // '<' (End delimiter)
inline constexpr uint8_t kEscapeSentry    = 0x3D; // '=' (Escape sentry)
inline constexpr uint8_t kEscaped3C       = 0x2C; // 0x3C is escaped as 0x3D 0x2C
inline constexpr uint8_t kEscaped3D       = 0x2D; // 0x3D is escaped as 0x3D 0x2D
inline constexpr uint8_t kEscaped3E       = 0x2E; // 0x3E is escaped as 0x3D 0x2E

// ---------------------------------------------------------------------------
// Device limits (WH-1000XM3)
// ---------------------------------------------------------------------------
// The XM3 exposes noise control as one continuous axis rather than as discrete
// modes: step 0 is full (dual-sensor) noise cancelling, step 1 is wind noise
// reduction (single-sensor NC), and steps 2..kMaxAmbientStep are ambient sound
// at increasing passthrough. kMaxAmbientStep is the fallback used until the
// headset answers NCASM_GET_CAPABILITY with its real step count.
inline constexpr uint8_t kMaxAmbientStep    = 19;
inline constexpr uint8_t kStepNoiseCancel   = 0;  // dual-sensor NC
inline constexpr uint8_t kStepWindReduction = 1;  // single-sensor NC
inline constexpr uint8_t kMinAmbientStep    = 2;  // lowest true ambient step
// Focus on Voice is only accepted by the headset at ambient step 2 and above.
inline constexpr uint8_t kMinVoiceFocusStep = 2;

// ---------------------------------------------------------------------------
// Protocol Enumerations
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t {
    ACK          = 0x01,
    DATA_MDR     = 0x0C,
    DATA_MDR_NO2 = 0x0E,
    UNKNOWN      = 0xFF
};

// v1 T1 command bytes (payload[0]).
enum class Command : uint8_t {
    CONNECT_GET_PROTOCOL_INFO   = 0x00,
    CONNECT_RET_PROTOCOL_INFO   = 0x01,
    CONNECT_GET_CAPABILITY_INFO = 0x02,
    CONNECT_RET_CAPABILITY_INFO = 0x03,
    CONNECT_GET_DEVICE_INFO     = 0x04,
    CONNECT_RET_DEVICE_INFO     = 0x05,
    CONNECT_GET_SUPPORT_FUNCTION= 0x06,
    CONNECT_RET_SUPPORT_FUNCTION= 0x07,

    COMMON_GET_BATTERY_LEVEL    = 0x10,
    COMMON_RET_BATTERY_LEVEL    = 0x11,
    COMMON_NTFY_BATTERY_LEVEL   = 0x13,
    COMMON_GET_UPSCALING_EFFECT = 0x14,
    COMMON_RET_UPSCALING_EFFECT = 0x15,
    COMMON_NTFY_UPSCALING_EFFECT= 0x17,
    COMMON_GET_AUDIO_CODEC      = 0x18,
    COMMON_RET_AUDIO_CODEC      = 0x19,
    COMMON_NTFY_AUDIO_CODEC     = 0x1B,

    VPT_GET_PARAM               = 0x46,
    VPT_RET_PARAM               = 0x47,
    VPT_SET_PARAM               = 0x48,
    VPT_NTFY_PARAM              = 0x49,

    EQEBB_GET_PARAM             = 0x56,
    EQEBB_RET_PARAM             = 0x57,
    EQEBB_SET_PARAM             = 0x58,
    EQEBB_NTFY_PARAM            = 0x59,

    NCASM_GET_CAPABILITY        = 0x60,
    NCASM_RET_CAPABILITY        = 0x61,
    NCASM_GET_PARAM             = 0x66,
    NCASM_RET_PARAM             = 0x67,
    NCASM_SET_PARAM             = 0x68,
    NCASM_NTFY_PARAM            = 0x69,

    AUDIO_GET_PARAM             = 0xE6,
    AUDIO_RET_PARAM             = 0xE7,
    AUDIO_SET_PARAM             = 0xE8,
    AUDIO_NTFY_PARAM            = 0xE9,

    SYSTEM_GET_PARAM            = 0xF6,
    SYSTEM_RET_PARAM            = 0xF7,
    SYSTEM_SET_PARAM            = 0xF8,
    SYSTEM_NTFY_PARAM           = 0xF9
};

// payload[1] discriminators, per command family.
enum class BatteryInquiredType : uint8_t { BATTERY = 0x00, LEFT_RIGHT = 0x01, CRADLE = 0x02 };
enum class CommonInquiredType  : uint8_t { FIXED_VALUE = 0x00 };
enum class NcAsmInquiredType   : uint8_t { NO_USE = 0x00, NC_ONLY = 0x01, NC_AND_ASM = 0x02, ASM_ONLY = 0x03 };
enum class EqEbbInquiredType   : uint8_t { NO_USE = 0x00, PRESET_EQ = 0x01, EBB = 0x02 };
enum class AudioInquiredType   : uint8_t { NO_USE = 0x00, CONNECTION_MODE = 0x01, UPSCALING = 0x02 };
enum class VptInquiredType     : uint8_t { NO_USE = 0x00, VPT = 0x01, SOUND_POSITION = 0x02 };
enum class SystemInquiredType  : uint8_t {
    NO_USE = 0x00, VIBRATOR = 0x01, POWER_SAVING_MODE = 0x02,
    CONTROL_BY_WEARING = 0x03, AUTO_POWER_OFF = 0x04
};

// NC/ASM sub-fields.
enum class NcAsmEffect       : uint8_t { OFF = 0x00, ON = 0x01, ADJUST_IN_PROGRESS = 0x10, ADJUST_COMPLETE = 0x11 };
enum class NcAsmSettingType  : uint8_t { ON_OFF = 0x00, LEVEL_ADJUSTMENT = 0x01, DUAL_SINGLE_OFF = 0x02 };
enum class NcDualSingleValue : uint8_t { OFF = 0x00, SINGLE = 0x01, DUAL = 0x02 };
enum class AsmSettingType    : uint8_t { ON_OFF = 0x00, LEVEL_ADJUSTMENT = 0x01 };
enum class AsmId             : uint8_t { NORMAL = 0x00, VOICE = 0x01 };

// Sent as the ASM level when noise processing is switched off entirely.
inline constexpr uint8_t kAsmLevelDisabled = 0xFF;

enum class NoiseMode : uint8_t {
    OFF     = 0,
    ANC     = 1,
    AMBIENT = 2,
    WIND    = 3
};

// The XM3 preset list. 0xA0 is the app's "Manual" slot; 0xA1/0xA2 are the two
// user-saved slots. Values are shared with the v2 table.
enum class EqPreset : uint8_t {
    OFF     = 0x00,
    BRIGHT  = 0x10,
    EXCITED = 0x11,
    MELLOW  = 0x12,
    RELAXED = 0x13,
    VOCAL   = 0x14,
    TREBLE  = 0x15,
    BASS    = 0x16,
    SPEECH  = 0x17,
    CUSTOM  = 0xA0,
    USER1   = 0xA1,
    USER2   = 0xA2,
    UNKNOWN = 0xFF
};

// VPT (Surround) presets.
enum class SurroundPreset : uint8_t {
    OFF              = 0x00,
    OUTDOOR_FESTIVAL = 0x01,
    ARENA            = 0x02,
    CONCERT_HALL     = 0x03,
    CLUB             = 0x04,
    UNKNOWN          = 0xFF
};

// VPT Sound Position presets.
enum class SoundPosition : uint8_t {
    OFF         = 0x00,
    FRONT_LEFT  = 0x01,
    FRONT_RIGHT = 0x02,
    FRONT       = 0x03,
    REAR_LEFT   = 0x11,
    REAR_RIGHT  = 0x12,
    UNKNOWN     = 0xFF
};

// Auto power off timer selection.
enum class AutoPowerOff : uint8_t {
    AFTER_5_MIN   = 0x00,
    AFTER_30_MIN  = 0x01,
    AFTER_60_MIN  = 0x02,
    AFTER_180_MIN = 0x03,
    WHEN_REMOVED  = 0x10,
    DISABLED      = 0x11,
    UNKNOWN       = 0xFF
};

// Bluetooth link preference (LDAC bitrate priority).
enum class ConnectionMode : uint8_t {
    SOUND_QUALITY = 0x00,
    STABLE_LINK   = 0x01,
    UNKNOWN       = 0xFF
};

// ---------------------------------------------------------------------------
// Data Structures
// ---------------------------------------------------------------------------
struct UnpackedFrame {
    PacketType type{PacketType::UNKNOWN};
    uint8_t seq{0};
    std::vector<uint8_t> payload;
};

struct HeadphoneState {
    int schema_version = 1;
    bool connected = false;
    std::string device_name = "WH-1000XM3";
    int battery_level = -1;             // 0-100, or -1 if unknown
    bool battery_charging = false;
    std::string noise_mode = "anc";     // "anc", "wind", "ambient", "off"
    int ambient_sound_level = 0;        // 0..ambient_max_level, wire step value
    int ambient_max_level = kMaxAmbientStep; // reported by NCASM_RET_CAPABILITY
    bool voice_passthrough = false;     // Focus on Voice
    std::string eq_preset = "off";
    std::array<int, 5> eq_custom_bands = {0, 0, 0, 0, 0}; // [-10, 10]
    int clear_bass = 0;                 // [-10, 10]
    bool dsee_hx = false;               // DSEE HX setting (user's choice)
    // Whether DSEE HX is actually processing right now. The headset disables it
    // by itself on LDAC and while EQ or VPT is active, so this can be false
    // while dsee_hx is true.
    bool dsee_hx_active = false;
    std::string surround = "off";       // VPT preset
    std::string sound_position = "off"; // VPT sound position
    std::string auto_power_off = "unknown";
    std::string connection_mode = "unknown"; // "quality" | "stable"
    std::string codec = "";             // SBC / AAC / LDAC / aptX / aptX HD
    int64_t last_updated = 0;

    [[nodiscard]] std::string toJson() const;
    static HeadphoneState makeDisconnected();
};

// ---------------------------------------------------------------------------
// Low-Level Framing & Checksum
// ---------------------------------------------------------------------------
uint8_t calculateChecksum(std::span<const uint8_t> data) noexcept;
std::vector<uint8_t> escapeBytes(std::span<const uint8_t> unescaped);
std::vector<uint8_t> unescapeBytes(std::span<const uint8_t> escaped);

// Packs a complete frame ready for transmission over RFCOMM (delimited with 0x3E and 0x3C)
std::vector<uint8_t> packFrame(PacketType type, uint8_t seq, std::span<const uint8_t> payload);

// Unpacks a single complete frame enclosed by [0x3E ... 0x3C]
std::optional<UnpackedFrame> unpackFrame(std::span<const uint8_t> frameBytes);

// Stream Framer: extracts complete [0x3E ... 0x3C] frames from streaming socket input
class StreamFramer {
public:
    void append(std::span<const uint8_t> incoming);
    std::optional<std::vector<uint8_t>> nextFrame();
    void reset();
    [[nodiscard]] size_t bufferedBytes() const noexcept { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
};

// ---------------------------------------------------------------------------
// Noise-control step helpers
// ---------------------------------------------------------------------------
// Maps a wire step onto the mode name the UI shows.
std::string stepToNoiseMode(uint8_t step);
// Maps a mode onto its canonical wire step. For AMBIENT, `preferredLevel` is
// used when it is a legal ambient step, otherwise a mid-scale default.
uint8_t noiseModeToStep(NoiseMode mode, int preferredLevel, int maxStep = kMaxAmbientStep);

// ---------------------------------------------------------------------------
// Command Serializers (Host -> XM3)
// ---------------------------------------------------------------------------
std::vector<uint8_t> serializeACK(uint8_t rx_seq);

// The single NC/ASM setter every noise-control command funnels through.
// `step` is the wire step (0 = NC, 1 = wind, >=2 = ambient); when `enabled` is
// false the headset turns noise processing off entirely and `step` is ignored.
std::vector<uint8_t> serializeNcAsm(bool enabled, uint8_t step, bool voiceFocus, uint8_t seq = 0);

std::vector<uint8_t> serializeNoiseMode(NoiseMode mode, uint8_t ambientLevel = 0, bool voiceFocus = false, uint8_t seq = 0);
std::vector<uint8_t> serializeAmbientLevel(uint8_t level, bool voiceFocus = false, uint8_t seq = 0);
std::vector<uint8_t> serializeEqPreset(EqPreset preset, uint8_t seq = 0);
std::vector<uint8_t> serializeCustomEq(const std::array<int, 5>& bands, int clearBass, uint8_t seq = 0);
std::vector<uint8_t> serializeDsee(bool enabled, uint8_t seq = 0);
std::vector<uint8_t> serializeSurround(SurroundPreset preset, uint8_t seq = 0);
std::vector<uint8_t> serializeSoundPosition(SoundPosition position, uint8_t seq = 0);
std::vector<uint8_t> serializeAutoPowerOff(AutoPowerOff timer, uint8_t seq = 0);
std::vector<uint8_t> serializeConnectionMode(ConnectionMode mode, uint8_t seq = 0);

// ---------------------------------------------------------------------------
// Session handshake (Host -> XM3)
//
// The headset ACKs parameter queries sent before this handshake but does not
// answer them. Sony's app always opens with these three, in this order.
// ---------------------------------------------------------------------------
std::vector<uint8_t> serializeQueryProtocolInfo(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryCapabilityInfo(uint8_t seq = 0);
std::vector<uint8_t> serializeQuerySupportFunction(uint8_t seq = 0);

// ---------------------------------------------------------------------------
// Query Serializers (Host -> XM3 initialization)
// ---------------------------------------------------------------------------
std::vector<uint8_t> serializeQueryBattery(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryNoiseMode(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryNcAsmCapability(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryEq(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryDsee(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryUpscalingEffect(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryCodec(uint8_t seq = 0);
std::vector<uint8_t> serializeQuerySurround(uint8_t seq = 0);
std::vector<uint8_t> serializeQuerySoundPosition(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryAutoPowerOff(uint8_t seq = 0);
std::vector<uint8_t> serializeQueryConnectionMode(uint8_t seq = 0);

// ---------------------------------------------------------------------------
// Inbound State Deserializer (XM3 -> Host)
// ---------------------------------------------------------------------------
// Parses an unpacked payload into HeadphoneState. Returns true if state was updated.
bool parseInboundPayload(std::span<const uint8_t> payload, HeadphoneState& state);

// ---------------------------------------------------------------------------
// Enum <-> String Helpers
// ---------------------------------------------------------------------------
std::string eqPresetToString(EqPreset preset);
EqPreset stringToEqPreset(const std::string& str);
std::string noiseModeToString(NoiseMode mode);
NoiseMode stringToNoiseMode(const std::string& str);
std::string surroundToString(SurroundPreset preset);
SurroundPreset stringToSurround(const std::string& str);
std::string soundPositionToString(SoundPosition position);
SoundPosition stringToSoundPosition(const std::string& str);
std::string autoPowerOffToString(AutoPowerOff timer);
AutoPowerOff stringToAutoPowerOff(const std::string& str);
std::string connectionModeToString(ConnectionMode mode);
ConnectionMode stringToConnectionMode(const std::string& str);
std::string codecToString(uint8_t codecByte);

} // namespace omarchy::sony::protocol

// Provide convenient alias for consumer code
namespace sony = omarchy::sony;
