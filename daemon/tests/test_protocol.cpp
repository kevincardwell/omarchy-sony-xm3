#include "MDRProtocolV1.hpp"
#include "BluetoothManager.hpp"
#include "StateEngine.hpp"
#include "IpcServer.hpp"
#include "CommandQueue.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <thread>
#include <chrono>
#include <atomic>

using namespace omarchy::sony;
using namespace omarchy::sony::protocol;

static int gFailedTests = 0;
static int gTotalTests = 0;

#define TEST_CASE(name) \
    std::cout << "[ RUN      ] " << name << std::endl; \
    gTotalTests++;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[  FAILED  ] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(actual, expected, msg) \
    do { \
        if ((actual) != (expected)) { \
            std::cerr << "[  FAILED  ] " << msg << " | Expected: " << (expected) \
                      << ", Actual: " << (actual) << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

#define TEST_PASS(name) \
    std::cout << "[       OK ] " << name << std::endl;

// ---------------------------------------------------------------------------
// 1. Checksum Verification Tests
// ---------------------------------------------------------------------------
void testChecksumCalculation() {
    TEST_CASE("ChecksumCalculation");

    // Framing and checksum are identical across MDR v1 and v2, so this vector
    // captured from a v2 device is still a valid check of the shared arithmetic.
    std::vector<uint8_t> data = {0x0C, 0x01, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x03, 0x00, 0x30, 0x18, 0x00, 0x00};
    uint8_t csum = calculateChecksum(data);
    // 12 + 1 + 8 + 1 + 3 + 48 + 24 = 97 = 0x61
    TEST_ASSERT_EQ(csum, 0x61, "Checksum must match physical capture modulo 256 sum");

    // Test modulo 256 overflow
    std::vector<uint8_t> overflowData = {0xFF, 0x02}; // 255 + 2 = 257 = 1 (mod 256)
    TEST_ASSERT_EQ(calculateChecksum(overflowData), 0x01, "Checksum must wrap at 256");

    // Empty vector
    std::vector<uint8_t> emptyData;
    TEST_ASSERT_EQ(calculateChecksum(emptyData), 0x00, "Empty checksum is 0");

    TEST_PASS("ChecksumCalculation");
}

// ---------------------------------------------------------------------------
// 2. Escaping & Delimiter Sentry Tests
// ---------------------------------------------------------------------------
void testEscapingAndUnescaping() {
    TEST_CASE("EscapingAndUnescaping");

    // Bytes containing delimiters 0x3C, 0x3D, 0x3E
    std::vector<uint8_t> raw = {0x10, 0x3C, 0x20, 0x3D, 0x30, 0x3E, 0x40};
    auto escaped = escapeBytes(raw);

    // 0x3C -> 0x3D 0x2C
    // 0x3D -> 0x3D 0x2D
    // 0x3E -> 0x3D 0x2E
    // Original 7 bytes with 3 escaped bytes should result in 7 + 3 = 10 bytes
    TEST_ASSERT_EQ(escaped.size(), static_cast<size_t>(10), "Escaped length should account for sentry expansions");

    auto unescaped = unescapeBytes(escaped);
    TEST_ASSERT(unescaped == raw, "Roundtrip escape and unescape must reproduce original bytes");

    // Edge case: Corrupt trailing escape sentry 0x3D at end of buffer
    std::vector<uint8_t> corruptTrailing = {0x01, 0x3D};
    auto corruptUnescape = unescapeBytes(corruptTrailing);
    TEST_ASSERT(corruptUnescape.empty(), "Unescape must reject trailing dangling escape sentry");

    // Edge case: Invalid escape sequence (0x3D followed by unexpected byte)
    std::vector<uint8_t> corruptInvalid = {0x01, 0x3D, 0xAA, 0x02};
    auto corruptInvalidUnescape = unescapeBytes(corruptInvalid);
    TEST_ASSERT(corruptInvalidUnescape.empty(), "Unescape must reject invalid escape sequence");

    TEST_PASS("EscapingAndUnescaping");
}

// ---------------------------------------------------------------------------
// 3. Packet Packing, Unpacking & Integrity Tests
// ---------------------------------------------------------------------------
void testPacketFraming() {
    TEST_CASE("PacketFraming");

    std::vector<uint8_t> payload = {0x68, 0x17, 0x01, 0x01, 0x00, 0x00, 0x00};
    auto frame = packFrame(PacketType::DATA_MDR, 1, payload);

    TEST_ASSERT(frame.size() >= 9, "Frame must be at least 9 bytes");
    TEST_ASSERT_EQ(frame.front(), kStartMarker, "Frame must start with 0x3E");
    TEST_ASSERT_EQ(frame.back(), kEndMarker, "Frame must end with 0x3C");

    // Unpack valid frame
    auto unpacked = unpackFrame(frame);
    TEST_ASSERT(unpacked.has_value(), "Valid frame must unpack successfully");
    TEST_ASSERT(unpacked->type == PacketType::DATA_MDR, "Packet type must match DATA_MDR");
    TEST_ASSERT_EQ(unpacked->seq, 1, "Sequence number must match");
    TEST_ASSERT(unpacked->payload == payload, "Unpacked payload must match original");

    // Corrupted checksum test: tamper with one byte in the frame
    std::vector<uint8_t> tampered = frame;
    tampered[4] ^= 0xFF;
    auto tamperedUnpack = unpackFrame(tampered);
    TEST_ASSERT(!tamperedUnpack.has_value(), "Tampered frame must fail checksum verification");

    // Missing start marker
    std::vector<uint8_t> noStart = frame;
    noStart[0] = 0x00;
    TEST_ASSERT(!unpackFrame(noStart).has_value(), "Missing start marker must be rejected");

    // Missing end marker
    std::vector<uint8_t> noEnd = frame;
    noEnd.back() = 0x00;
    TEST_ASSERT(!unpackFrame(noEnd).has_value(), "Missing end marker must be rejected");

    // Too short frame (< 9 bytes)
    std::vector<uint8_t> tooShort = {0x3E, 0x0C, 0x3C};
    TEST_ASSERT(!unpackFrame(tooShort).has_value(), "Short frame must be rejected");

    TEST_PASS("PacketFraming");
}

// ---------------------------------------------------------------------------
// 4. StreamFramer Chunk Fragmentation & Coalescing Tests
// ---------------------------------------------------------------------------
void testStreamFramer() {
    TEST_CASE("StreamFramer");

    StreamFramer framer;
    std::vector<uint8_t> payload1 = {0xAA, 0xBB, 0xCC};
    std::vector<uint8_t> payload2 = {0x11, 0x22, 0x33, 0x44};

    auto frame1 = packFrame(PacketType::DATA_MDR, 0, payload1);
    auto frame2 = packFrame(PacketType::DATA_MDR, 1, payload2);

    // Test 1: Fragmented feed - 1 byte at a time
    for (size_t i = 0; i < frame1.size() - 1; ++i) {
        uint8_t b = frame1[i];
        framer.append(std::span<const uint8_t>(&b, 1));
        auto pending = framer.nextFrame();
        TEST_ASSERT(!pending.has_value(), "Frame must not complete before final delimiter");
    }

    // Feed the last byte
    uint8_t lastByte = frame1.back();
    framer.append(std::span<const uint8_t>(&lastByte, 1));
    auto extracted1 = framer.nextFrame();
    TEST_ASSERT(extracted1.has_value(), "Frame 1 must complete on final byte");
    TEST_ASSERT(*extracted1 == frame1, "Extracted frame must match frame 1 exactly");

    // Test 2: Coalesced packets + leading garbage
    std::vector<uint8_t> combined;
    combined.push_back(0xFF); // Garbage preceding start
    combined.push_back(0xDE);
    combined.insert(combined.end(), frame1.begin(), frame1.end());
    combined.insert(combined.end(), frame2.begin(), frame2.end());

    framer.append(combined);
    auto res1 = framer.nextFrame();
    TEST_ASSERT(res1.has_value(), "First coalesced frame must be extracted");
    TEST_ASSERT(*res1 == frame1, "First frame content must match");

    auto res2 = framer.nextFrame();
    TEST_ASSERT(res2.has_value(), "Second coalesced frame must be extracted");
    TEST_ASSERT(*res2 == frame2, "Second frame content must match");

    auto res3 = framer.nextFrame();
    TEST_ASSERT(!res3.has_value(), "Framer must be empty after all frames extracted");

    // Reset test
    framer.append(frame1);
    framer.reset();
    TEST_ASSERT_EQ(framer.bufferedBytes(), static_cast<size_t>(0), "Framer buffer must be empty after reset");

    TEST_PASS("StreamFramer");
}


// ---------------------------------------------------------------------------
// 5. Command Serializer Tests (MDR v1 / WH-1000XM3 command table)
// ---------------------------------------------------------------------------
//
// Every expectation below is a byte-for-byte assertion against the v1 T1 table.
// If one of these starts failing after a refactor, the daemon is speaking the
// XM5's v2 table at an XM3, which the headset silently ignores.
// ---------------------------------------------------------------------------

// Extracts the payload from a fully framed packet, i.e. drops the markers,
// header, checksum, and any escaping.
static std::vector<uint8_t> payloadOf(const std::vector<uint8_t>& frame) {
    auto unpacked = unpackFrame(frame);
    if (!unpacked) return {};
    return unpacked->payload;
}

void testCommandSerializers() {
    TEST_CASE("CommandSerializers");

    // --- Noise cancelling (wire step 0 => NcDualSingleValue::DUAL) ----------
    {
        auto p = payloadOf(serializeNoiseMode(NoiseMode::ANC, 0, false, 0));
        std::vector<uint8_t> expect = {
            0x68, // NCASM_SET_PARAM
            0x02, // NOISE_CANCELLING_AND_AMBIENT_SOUND_MODE
            0x11, // NcAsmEffect::ADJUST_COMPLETE
            0x01, // NcAsmSettingType::LEVEL_ADJUSTMENT
            0x02, // NcDualSingleValue::DUAL
            0x01, // AsmSettingType::LEVEL_ADJUSTMENT
            0x00, // AsmId::NORMAL
            0x00  // asmLevel
        };
        TEST_ASSERT(p == expect, "ANC must serialize as an 8-byte v1 NC/ASM command with ncValue=DUAL");
    }

    // --- Wind noise reduction (wire step 1 => SINGLE) -----------------------
    {
        auto p = payloadOf(serializeNoiseMode(NoiseMode::WIND, 0, false, 0));
        TEST_ASSERT_EQ(p.size(), static_cast<size_t>(8), "Wind command must be 8 bytes");
        TEST_ASSERT_EQ(static_cast<int>(p[4]), 0x01, "Wind reduction uses NcDualSingleValue::SINGLE");
        TEST_ASSERT_EQ(static_cast<int>(p[7]), 0x01, "Wind reduction is ambient step 1");
    }

    // --- Ambient sound at an explicit level --------------------------------
    {
        auto p = payloadOf(serializeAmbientLevel(14, false, 0));
        TEST_ASSERT_EQ(static_cast<int>(p[4]), 0x00, "Ambient uses NcDualSingleValue::OFF");
        TEST_ASSERT_EQ(static_cast<int>(p[6]), 0x00, "AsmId must be NORMAL without Focus on Voice");
        TEST_ASSERT_EQ(static_cast<int>(p[7]), 14, "Ambient level must be carried verbatim");
    }

    // --- Focus on Voice ----------------------------------------------------
    {
        auto p = payloadOf(serializeAmbientLevel(10, true, 0));
        TEST_ASSERT_EQ(static_cast<int>(p[6]), 0x01, "AsmId must be VOICE when Focus on Voice is on");

        // Below step 2 the headset rejects Focus on Voice, so it must be dropped.
        auto low = payloadOf(serializeAmbientLevel(1, true, 0));
        TEST_ASSERT_EQ(static_cast<int>(low[6]), 0x00,
                       "Focus on Voice must be suppressed below ambient step 2");
    }

    // --- Noise processing off ----------------------------------------------
    {
        auto p = payloadOf(serializeNoiseMode(NoiseMode::OFF, 0, false, 0));
        TEST_ASSERT_EQ(static_cast<int>(p[2]), 0x00, "Off uses NcAsmEffect::OFF");
        TEST_ASSERT_EQ(static_cast<int>(p[7]), 0xFF, "Off sends the disabled ASM level sentinel");
    }

    // --- Ambient mode with no remembered level lands mid-scale -------------
    {
        auto p = payloadOf(serializeNoiseMode(NoiseMode::AMBIENT, 0, false, 0));
        const int level = p[7];
        TEST_ASSERT(level >= kMinAmbientStep && level <= kMaxAmbientStep,
                    "Ambient with no preferred level must still be a legal ambient step");
    }

    // --- EQ preset (v1 uses PRESET_EQ = 0x01, not the v2 value of 0x00) ----
    {
        auto p = payloadOf(serializeEqPreset(EqPreset::BASS, 0));
        std::vector<uint8_t> expect = {0x58, 0x01, 0x16, 0x00};
        TEST_ASSERT(p == expect, "EQ preset must use EQEBB_SET_PARAM with PRESET_EQ=0x01");
    }

    // --- Custom EQ: 6 band steps, Clear Bass first, each biased by +10 -----
    {
        std::array<int, 5> bands = {-10, -5, 0, 5, 10};
        auto p = payloadOf(serializeCustomEq(bands, 3, 0));
        std::vector<uint8_t> expect = {
            0x58, 0x01, 0xA0, 0x06,
            13,        // clear bass  3 + 10
            0, 5, 10, 15, 20
        };
        TEST_ASSERT(p == expect, "Custom EQ must send [ClearBass, b0..b4] biased by +10");

        // Out-of-range input must clamp rather than wrap around.
        std::array<int, 5> wild = {-99, 99, 0, 0, 0};
        auto clamped = payloadOf(serializeCustomEq(wild, 99, 0));
        TEST_ASSERT_EQ(static_cast<int>(clamped[5]), 0,  "Band below -10 must clamp to 0");
        TEST_ASSERT_EQ(static_cast<int>(clamped[6]), 20, "Band above +10 must clamp to 20");
        TEST_ASSERT_EQ(static_cast<int>(clamped[4]), 20, "Clear Bass above +10 must clamp to 20");
    }

    // --- DSEE HX: AUDIO_SET_PARAM with UPSCALING = 0x02 on v1 --------------
    {
        auto on  = payloadOf(serializeDsee(true, 0));
        auto off = payloadOf(serializeDsee(false, 0));
        std::vector<uint8_t> expectOn = {0xE8, 0x02, 0x00, 0x01};
        TEST_ASSERT(on == expectOn, "DSEE HX on must be [0xE8, 0x02, 0x00, 0x01]");
        TEST_ASSERT_EQ(static_cast<int>(off[3]), 0x00, "DSEE HX off must send value 0");
    }

    // --- Session handshake ---------------------------------------------------
    //     Without these the XM3 ACKs parameter queries but never answers them
    //     (observed on hardware, 2026-09-11).
    {
        std::vector<uint8_t> protocol = {0x00, 0x00};
        std::vector<uint8_t> capability = {0x02, 0x00};
        std::vector<uint8_t> support = {0x06, 0x00};
        TEST_ASSERT(payloadOf(serializeQueryProtocolInfo(0)) == protocol, "Handshake 1 is CONNECT_GET_PROTOCOL_INFO");
        TEST_ASSERT(payloadOf(serializeQueryCapabilityInfo(0)) == capability, "Handshake 2 is CONNECT_GET_CAPABILITY_INFO");
        TEST_ASSERT(payloadOf(serializeQuerySupportFunction(0)) == support, "Handshake 3 is CONNECT_GET_SUPPORT_FUNCTION");
    }

    // --- VPT surround and sound position -----------------------------------
    {
        auto surround = payloadOf(serializeSurround(SurroundPreset::CONCERT_HALL, 0));
        std::vector<uint8_t> expectSurround = {0x48, 0x01, 0x03};
        TEST_ASSERT(surround == expectSurround, "Surround must be VPT_SET_PARAM / VPT / preset");

        auto position = payloadOf(serializeSoundPosition(SoundPosition::REAR_RIGHT, 0));
        std::vector<uint8_t> expectPosition = {0x48, 0x02, 0x12};
        TEST_ASSERT(position == expectPosition, "Sound position must be VPT_SET_PARAM / SOUND_POSITION / preset");
    }

    // --- Auto power off -----------------------------------------------------
    {
        auto p = payloadOf(serializeAutoPowerOff(AutoPowerOff::AFTER_30_MIN, 0));
        std::vector<uint8_t> expect = {0xF8, 0x04, 0x01, 0x01, 0x01};
        TEST_ASSERT(p == expect, "Auto power off must carry active and select-time element ids");

        // Disabling must keep a real timer in the select slot.
        auto disabled = payloadOf(serializeAutoPowerOff(AutoPowerOff::DISABLED, 0));
        TEST_ASSERT_EQ(static_cast<int>(disabled[3]), 0x11, "Disabled sets the active element to 0x11");
        TEST_ASSERT_EQ(static_cast<int>(disabled[4]), 0x03, "Disabled keeps a concrete select-time element");
    }

    // --- Connection mode (LDAC bitrate priority) ---------------------------
    {
        auto stable = payloadOf(serializeConnectionMode(ConnectionMode::STABLE_LINK, 0));
        std::vector<uint8_t> expect = {0xE8, 0x01, 0x00, 0x01};
        TEST_ASSERT(stable == expect, "Connection mode must be AUDIO_SET_PARAM / CONNECTION_MODE");
    }

    // --- ACK toggles the sequence number ------------------------------------
    {
        auto ack0 = unpackFrame(serializeACK(0));
        auto ack1 = unpackFrame(serializeACK(1));
        TEST_ASSERT(ack0.has_value() && ack1.has_value(), "ACK frames must unpack");
        TEST_ASSERT_EQ(static_cast<int>(ack0->seq), 1, "ACK for seq 0 must carry seq 1");
        TEST_ASSERT_EQ(static_cast<int>(ack1->seq), 0, "ACK for seq 1 must carry seq 0");
        TEST_ASSERT(ack0->type == PacketType::ACK, "ACK frame type must be 0x01");
        TEST_ASSERT(ack0->payload.empty(), "ACK carries no payload");
    }

    TEST_PASS("CommandSerializers");
}

// ---------------------------------------------------------------------------
// 6. Query Serializer Tests
// ---------------------------------------------------------------------------
void testQuerySerializers() {
    TEST_CASE("QuerySerializers");

    struct Case {
        std::vector<uint8_t> frame;
        std::vector<uint8_t> expect;
        const char* label;
    };

    const std::vector<Case> cases = {
        {serializeQueryBattery(0),          {0x10, 0x00}, "battery"},
        {serializeQueryNoiseMode(0),        {0x66, 0x02}, "noise mode"},
        {serializeQueryNcAsmCapability(0),  {0x60, 0x02}, "nc/asm capability"},
        {serializeQueryEq(0),               {0x56, 0x01}, "equalizer"},
        {serializeQueryDsee(0),             {0xE6, 0x02}, "dsee hx"},
        {serializeQueryUpscalingEffect(0),  {0x14, 0x00}, "upscaling effect"},
        {serializeQueryCodec(0),            {0x18, 0x00}, "audio codec"},
        {serializeQuerySurround(0),         {0x46, 0x01}, "surround"},
        {serializeQuerySoundPosition(0),    {0x46, 0x02}, "sound position"},
        {serializeQueryAutoPowerOff(0),     {0xF6, 0x04}, "auto power off"},
        {serializeQueryConnectionMode(0),   {0xE6, 0x01}, "connection mode"},
    };

    for (const auto& c : cases) {
        auto p = payloadOf(c.frame);
        TEST_ASSERT(p == c.expect, std::string("Query payload mismatch for ") + c.label);
    }

    TEST_PASS("QuerySerializers");
}

// ---------------------------------------------------------------------------
// 7. Inbound State Parser Tests
// ---------------------------------------------------------------------------
void testInboundStateParser() {
    TEST_CASE("InboundStateParser");

    // --- Battery: COMMON_RET_BATTERY_LEVEL ---------------------------------
    {
        HeadphoneState s;
        std::vector<uint8_t> p = {0x11, 0x00, 72, 0x01};
        TEST_ASSERT(parseInboundPayload(p, s), "Battery return must update state");
        TEST_ASSERT_EQ(s.battery_level, 72, "Battery level must be parsed");
        TEST_ASSERT(s.battery_charging, "Charging flag must be parsed");
        TEST_ASSERT(s.connected, "Any successful parse marks the headset connected");

        // The notify variant carries the same shape under a different command.
        HeadphoneState n;
        std::vector<uint8_t> np = {0x13, 0x00, 15, 0x00};
        TEST_ASSERT(parseInboundPayload(np, n), "Battery notify must update state");
        TEST_ASSERT_EQ(n.battery_level, 15, "Battery notify level must be parsed");
        TEST_ASSERT(!n.battery_charging, "Not-charging must be parsed");
    }

    // --- Codec: COMMON_RET_AUDIO_CODEC -------------------------------------
    {
        HeadphoneState s;
        std::vector<uint8_t> p = {0x19, 0x00, 0x10};
        TEST_ASSERT(parseInboundPayload(p, s), "Codec return must update state");
        TEST_ASSERT(s.codec == "LDAC", "Codec 0x10 is LDAC");

        HeadphoneState a;
        std::vector<uint8_t> ap = {0x19, 0x00, 0x21};
        parseInboundPayload(ap, a);
        TEST_ASSERT(a.codec == "aptX HD", "Codec 0x21 is aptX HD");
    }

    // --- Noise control: NCASM_RET_PARAM ------------------------------------
    {
        // ncValue = DUAL => noise cancelling
        HeadphoneState anc;
        std::vector<uint8_t> ancP = {0x67, 0x02, 0x11, 0x01, 0x02, 0x01, 0x00, 0x00};
        TEST_ASSERT(parseInboundPayload(ancP, anc), "NC/ASM return must update state");
        TEST_ASSERT(anc.noise_mode == "anc", "ncValue DUAL must map to anc");

        // ncValue = SINGLE => wind noise reduction
        HeadphoneState wind;
        std::vector<uint8_t> windP = {0x67, 0x02, 0x11, 0x01, 0x01, 0x01, 0x00, 0x01};
        parseInboundPayload(windP, wind);
        TEST_ASSERT(wind.noise_mode == "wind", "ncValue SINGLE must map to wind");

        // ncValue = OFF => ambient sound at the reported level
        HeadphoneState amb;
        std::vector<uint8_t> ambP = {0x67, 0x02, 0x11, 0x01, 0x00, 0x01, 0x01, 17};
        parseInboundPayload(ambP, amb);
        TEST_ASSERT(amb.noise_mode == "ambient", "ncValue OFF must map to ambient");
        TEST_ASSERT_EQ(amb.ambient_sound_level, 17, "Ambient level must be parsed");
        TEST_ASSERT(amb.voice_passthrough, "AsmId VOICE must set voice passthrough");

        // ncAsmEffect = OFF => noise processing off, whatever the other fields say
        HeadphoneState off;
        std::vector<uint8_t> offP = {0x67, 0x02, 0x00, 0x01, 0x02, 0x01, 0x00, 0xFF};
        parseInboundPayload(offP, off);
        TEST_ASSERT(off.noise_mode == "off", "NcAsmEffect OFF must map to off");
    }

    // --- NC/ASM capability gives the real ambient step ceiling ---------------
    {
        HeadphoneState s;
        // [cmd, type, ncSettingType, ncStep, asmSettingType, count, (id, steps) x2]
        std::vector<uint8_t> p = {0x61, 0x02, 0x02, 0x02, 0x01, 0x02,
                                  0x00, 20,    // NORMAL: 20 steps => max index 19
                                  0x01, 20};   // VOICE
        TEST_ASSERT(parseInboundPayload(p, s), "NC/ASM capability must update state");
        TEST_ASSERT_EQ(s.ambient_max_level, 19, "20 reported steps means a top index of 19");
    }

    // --- Equalizer: EQEBB_RET_PARAM ----------------------------------------
    {
        HeadphoneState s;
        std::vector<uint8_t> p = {0x57, 0x01, 0xA0, 0x06, 13, 0, 5, 10, 15, 20};
        TEST_ASSERT(parseInboundPayload(p, s), "EQ return must update state");
        TEST_ASSERT(s.eq_preset == "custom", "Preset 0xA0 is the custom/manual slot");
        TEST_ASSERT_EQ(s.clear_bass, 3, "Clear Bass must be de-biased by 10");
        TEST_ASSERT_EQ(s.eq_custom_bands[0], -10, "Band 0 must be de-biased by 10");
        TEST_ASSERT_EQ(s.eq_custom_bands[4], 10, "Band 4 must be de-biased by 10");

        // A preset with no band steps must still update the preset name.
        HeadphoneState bare;
        std::vector<uint8_t> bareP = {0x57, 0x01, 0x16, 0x00};
        parseInboundPayload(bareP, bare);
        TEST_ASSERT(bare.eq_preset == "bass", "Preset 0x16 is bass");
    }

    // --- VPT ----------------------------------------------------------------
    {
        HeadphoneState s;
        std::vector<uint8_t> surround = {0x47, 0x01, 0x02};
        parseInboundPayload(surround, s);
        TEST_ASSERT(s.surround == "arena", "VPT preset 0x02 is arena");

        std::vector<uint8_t> position = {0x47, 0x02, 0x11};
        parseInboundPayload(position, s);
        TEST_ASSERT(s.sound_position == "rear-left", "Sound position 0x11 is rear-left");
    }

    // --- Audio params: DSEE HX and connection mode --------------------------
    {
        HeadphoneState s;
        std::vector<uint8_t> dsee = {0xE7, 0x02, 0x00, 0x01};
        parseInboundPayload(dsee, s);
        TEST_ASSERT(s.dsee_hx, "AUDIO_RET_PARAM UPSCALING value 1 enables DSEE HX");

        std::vector<uint8_t> conn = {0xE7, 0x01, 0x00, 0x01};
        parseInboundPayload(conn, s);
        TEST_ASSERT(s.connection_mode == "stable", "Connection mode 0x01 prioritises a stable link");
    }

    // --- System params: auto power off ---------------------------------------
    {
        HeadphoneState s;
        // The XM3 has no wearing sensor; a stray CONTROL_BY_WEARING frame must
        // be ignored rather than half-parsed into some other field.
        std::vector<uint8_t> wearing = {0xF7, 0x03, 0x00, 0x00};
        TEST_ASSERT(!parseInboundPayload(wearing, s), "CONTROL_BY_WEARING is not an XM3 setting");

        std::vector<uint8_t> apo = {0xF7, 0x04, 0x01, 0x11, 0x03};
        parseInboundPayload(apo, s);
        TEST_ASSERT(s.auto_power_off == "off", "Auto power off element 0x11 is disabled");
    }

    // --- Real WH-1000XM3 traffic, captured 2026-09-11 ------------------------
    {
        HeadphoneState s;

        std::vector<uint8_t> cap = {0x61, 0x02, 0x02, 0x00, 0x01, 0x02, 0x00, 0x14, 0x01, 0x14};
        TEST_ASSERT(parseInboundPayload(cap, s), "Captured NC/ASM capability must parse");
        TEST_ASSERT_EQ(s.ambient_max_level, 19, "Captured capability: 20 steps, top index 19");

        std::vector<uint8_t> battery = {0x11, 0x00, 0x46, 0x00};
        parseInboundPayload(battery, s);
        TEST_ASSERT_EQ(s.battery_level, 70, "Captured battery: 70%");

        // The headset reports effect ON (0x01) and ncType DUAL_SINGLE_OFF (0x02)
        // even though we send ADJUST_COMPLETE and LEVEL_ADJUSTMENT.
        std::vector<uint8_t> anc = {0x67, 0x02, 0x01, 0x02, 0x02, 0x01, 0x00, 0x00};
        parseInboundPayload(anc, s);
        TEST_ASSERT(s.noise_mode == "anc", "Captured NC/ASM reply decodes as ANC");

        std::vector<uint8_t> ambient = {0x69, 0x02, 0x01, 0x02, 0x00, 0x01, 0x01, 0x0a};
        parseInboundPayload(ambient, s);
        TEST_ASSERT(s.noise_mode == "ambient", "Captured notify decodes as ambient");
        TEST_ASSERT_EQ(s.ambient_sound_level, 10, "Captured notify: ambient step 10");
        TEST_ASSERT(s.voice_passthrough, "Captured notify: Focus on Voice on");

        std::vector<uint8_t> codec = {0x1b, 0x00, 0x10};
        parseInboundPayload(codec, s);
        TEST_ASSERT(s.codec == "LDAC", "Captured codec notify: LDAC");

        std::vector<uint8_t> mode = {0xe7, 0x01, 0x00, 0x01};
        parseInboundPayload(mode, s);
        TEST_ASSERT(s.connection_mode == "stable", "Captured connection mode: stable");

        // DSEE HX switched on, then the headset reports it INVALID because the
        // link is LDAC. The setting must survive; only the activity flag drops.
        std::vector<uint8_t> dseeOn = {0xe9, 0x02, 0x00, 0x01};
        std::vector<uint8_t> dseeInvalid = {0x17, 0x00, 0x00, 0x02};
        parseInboundPayload(dseeOn, s);
        parseInboundPayload(dseeInvalid, s);
        TEST_ASSERT(s.dsee_hx, "An INVALID upscaling indicator must not switch the DSEE HX setting off");
        TEST_ASSERT(!s.dsee_hx_active, "INVALID upscaling indicator means DSEE HX is not processing");

        std::vector<uint8_t> dseeValid = {0x17, 0x00, 0x00, 0x01};
        parseInboundPayload(dseeValid, s);
        TEST_ASSERT(s.dsee_hx_active, "VALID upscaling indicator means DSEE HX is processing");

        std::vector<uint8_t> eqBass = {0x59, 0x01, 0x16, 0x06, 0x11, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a};
        parseInboundPayload(eqBass, s);
        TEST_ASSERT(s.eq_preset == "bass", "Captured EQ notify: Bass Boost");
        TEST_ASSERT_EQ(s.clear_bass, 7, "Captured EQ notify: Bass Boost carries Clear Bass +7");
    }

    // --- Malformed and unknown payloads must be rejected, not crash ---------
    {
        HeadphoneState s;
        std::vector<uint8_t> empty;
        TEST_ASSERT(!parseInboundPayload(empty, s), "Empty payload must not update state");

        std::vector<uint8_t> truncated = {0x11, 0x00};
        TEST_ASSERT(!parseInboundPayload(truncated, s), "Truncated battery payload must be rejected");

        std::vector<uint8_t> shortNcAsm = {0x67, 0x02, 0x11, 0x01};
        TEST_ASSERT(!parseInboundPayload(shortNcAsm, s), "Truncated NC/ASM payload must be rejected");

        std::vector<uint8_t> unknown = {0x7B, 0x01, 0x02, 0x03};
        TEST_ASSERT(!parseInboundPayload(unknown, s), "Unknown command must not update state");

        // A v2-table battery response must not be mistaken for a v1 one.
        std::vector<uint8_t> v2Battery = {0x23, 0x00, 50, 0x00};
        TEST_ASSERT(!parseInboundPayload(v2Battery, s), "v2 battery command must not parse under v1");
    }

    TEST_PASS("InboundStateParser");
}

// ---------------------------------------------------------------------------
// 8. Enum, String & Step-Mapping Helper Tests
// ---------------------------------------------------------------------------
void testStringAndEnumHelpers() {
    TEST_CASE("StringAndEnumHelpers");

    // --- Round trips --------------------------------------------------------
    const char* modes[] = {"anc", "ambient", "wind", "off"};
    for (const char* m : modes) {
        TEST_ASSERT(noiseModeToString(stringToNoiseMode(m)) == m,
                    std::string("Noise mode round trip failed for ") + m);
    }

    const char* presets[] = {"off", "bright", "excited", "mellow", "relaxed",
                             "vocal", "treble", "bass", "speech", "custom", "user1", "user2"};
    for (const char* e : presets) {
        TEST_ASSERT(eqPresetToString(stringToEqPreset(e)) == e,
                    std::string("EQ preset round trip failed for ") + e);
    }

    const char* surrounds[] = {"off", "outdoor", "arena", "concert", "club"};
    for (const char* v : surrounds) {
        TEST_ASSERT(surroundToString(stringToSurround(v)) == v,
                    std::string("Surround round trip failed for ") + v);
    }

    const char* positions[] = {"off", "front-left", "front-right", "front", "rear-left", "rear-right"};
    for (const char* v : positions) {
        TEST_ASSERT(soundPositionToString(stringToSoundPosition(v)) == v,
                    std::string("Sound position round trip failed for ") + v);
    }

    const char* timers[] = {"off", "5min", "30min", "60min", "180min"};
    for (const char* v : timers) {
        TEST_ASSERT(autoPowerOffToString(stringToAutoPowerOff(v)) == v,
                    std::string("Auto power off round trip failed for ") + v);
    }

    TEST_ASSERT(connectionModeToString(stringToConnectionMode("quality")) == "quality",
                "Connection mode round trip failed for quality");
    TEST_ASSERT(connectionModeToString(stringToConnectionMode("stable")) == "stable",
                "Connection mode round trip failed for stable");

    // --- Unknown inputs fail closed rather than silently picking a value ----
    TEST_ASSERT(stringToSurround("stadium") == SurroundPreset::UNKNOWN, "Unknown surround must be UNKNOWN");
    TEST_ASSERT(stringToSoundPosition("above") == SoundPosition::UNKNOWN, "Unknown position must be UNKNOWN");
    TEST_ASSERT(stringToAutoPowerOff("7min") == AutoPowerOff::UNKNOWN, "Unknown timer must be UNKNOWN");
    TEST_ASSERT(stringToConnectionMode("fast") == ConnectionMode::UNKNOWN, "Unknown connection mode must be UNKNOWN");

    // "manual" is the Sony app's name for the 0xA0 custom slot.
    TEST_ASSERT(stringToEqPreset("manual") == EqPreset::CUSTOM, "'manual' must alias the custom preset");

    // --- Step <-> mode mapping ---------------------------------------------
    TEST_ASSERT(stepToNoiseMode(0) == "anc", "Step 0 is noise cancelling");
    TEST_ASSERT(stepToNoiseMode(1) == "wind", "Step 1 is wind noise reduction");
    TEST_ASSERT(stepToNoiseMode(2) == "ambient", "Step 2 is the lowest ambient step");
    TEST_ASSERT(stepToNoiseMode(19) == "ambient", "Step 19 is ambient");

    TEST_ASSERT_EQ(static_cast<int>(noiseModeToStep(NoiseMode::ANC, 12)), 0, "ANC always maps to step 0");
    TEST_ASSERT_EQ(static_cast<int>(noiseModeToStep(NoiseMode::WIND, 12)), 1, "Wind always maps to step 1");
    TEST_ASSERT_EQ(static_cast<int>(noiseModeToStep(NoiseMode::AMBIENT, 12)), 12,
                   "Ambient must honour a legal preferred level");
    TEST_ASSERT_EQ(static_cast<int>(noiseModeToStep(NoiseMode::AMBIENT, 99)), 10,
                   "Ambient must fall back to mid-scale for an out-of-range preference");
    TEST_ASSERT_EQ(static_cast<int>(noiseModeToStep(NoiseMode::AMBIENT, -1)), 10,
                   "Ambient must fall back to mid-scale when nothing is remembered");

    // --- Codec names --------------------------------------------------------
    TEST_ASSERT(codecToString(0x01) == "SBC", "0x01 is SBC");
    TEST_ASSERT(codecToString(0x02) == "AAC", "0x02 is AAC");
    TEST_ASSERT(codecToString(0x10) == "LDAC", "0x10 is LDAC");
    TEST_ASSERT(codecToString(0x20) == "aptX", "0x20 is aptX");
    TEST_ASSERT(codecToString(0x00).empty(), "0x00 (unsettled) has no name yet");

    TEST_PASS("StringAndEnumHelpers");
}

// ---------------------------------------------------------------------------
// 9. Mock Transport & BluetoothManager Lifecycle Tests
// ---------------------------------------------------------------------------
void testMockTransportAndBluetoothManager() {
    TEST_CASE("MockTransportAndBluetoothManager");

    int peerFd = -1;
    BluetoothConfig config;
    config.initialBackoffMs = 50;
    config.maxBackoffMs = 200;
    auto manager = BluetoothManager::createMock(config, &peerFd);
    TEST_ASSERT(manager != nullptr, "BluetoothManager must be created");
    TEST_ASSERT(peerFd >= 0, "Mock peer socketpair fd must be valid");

    bool connectedCalled = false;
    bool disconnectedCalled = false;
    std::vector<uint8_t> receivedBytes;

    BluetoothCallbacks callbacks;
    callbacks.onConnected = [&]() { connectedCalled = true; };
    callbacks.onDisconnected = [&](const std::string&) { disconnectedCalled = true; };
    callbacks.onDataReceived = [&](const uint8_t* data, size_t len) {
        receivedBytes.insert(receivedBytes.end(), data, data + len);
    };
    manager->setCallbacks(callbacks);

    // Initial state is DISCONNECTED
    TEST_ASSERT(manager->getState() == ConnectionState::DISCONNECTED, "Initial state DISCONNECTED");

    // Start -> discovery -> sdp -> connect
    manager->start();
    TEST_ASSERT(manager->getState() == ConnectionState::CONNECTED, "State must transition to CONNECTED in mock mode");
    TEST_ASSERT(connectedCalled, "onConnected callback must be triggered");

    // Verify polling fd
    int pollFd = manager->getPollFd();
    TEST_ASSERT(pollFd >= 0, "Poll fd must be valid when connected");
    short pollEvents = manager->getPollEvents();
    TEST_ASSERT(pollEvents & POLLIN, "Poll events must include POLLIN");

    // Test sending packet from manager to peerFd
    std::vector<uint8_t> testPacket = {0x3E, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x01, 0x55, 0x63, 0x3C};
    bool sendOk = manager->sendPacket(testPacket);
    TEST_ASSERT(sendOk, "sendPacket must succeed");

    // Read from peerFd and verify
    uint8_t readBuf[128];
    ssize_t nRead = ::recv(peerFd, readBuf, sizeof(readBuf), 0);
    TEST_ASSERT_EQ(nRead, static_cast<ssize_t>(testPacket.size()), "Peer must receive exact packet bytes");
    TEST_ASSERT(std::memcmp(readBuf, testPacket.data(), testPacket.size()) == 0, "Received bytes match sent packet");

    // Test sending data from peerFd to manager
    std::vector<uint8_t> inboundPacket = {0x3E, 0x0C, 0x00, 0x00, 0x00, 0x01, 0xAA, 0xB7, 0x3C};
    ssize_t nSent = ::send(peerFd, inboundPacket.data(), inboundPacket.size(), 0);
    TEST_ASSERT_EQ(nSent, static_cast<ssize_t>(inboundPacket.size()), "Peer sent bytes");

    // Trigger handleSocketEvent with POLLIN
    manager->handleSocketEvent(POLLIN);
    TEST_ASSERT(receivedBytes == inboundPacket, "Manager onDataReceived must receive inbound bytes");

    // Test remote disconnect simulation
    // Closing peerFd causes POLLHUP/EOF on manager side
    ::close(peerFd);
    peerFd = -1;

    manager->handleSocketEvent(POLLHUP);
    TEST_ASSERT(manager->getState() == ConnectionState::RECONNECT_BACKOFF, "State must transition to RECONNECT_BACKOFF on hangup");
    TEST_ASSERT(disconnectedCalled, "onDisconnected callback must be triggered");

    // Test backoff tick retry
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    manager->tick();
    // After backoff timer expires, tick() should transition through discovery and reconnect
    TEST_ASSERT(manager->getState() == ConnectionState::CONNECTED, "After backoff timer, tick must trigger reconnect to mock");

    TEST_PASS("MockTransportAndBluetoothManager");
}

// ---------------------------------------------------------------------------
// 10. StateEngine Persistence & Permissions Tests
// ---------------------------------------------------------------------------
void testStateEngine() {
    TEST_CASE("StateEngine");

    char tmpl[] = "/tmp/test_omasonyxm3_state_XXXXXX";
    char* sandbox = ::mkdtemp(tmpl);
    TEST_ASSERT(sandbox != nullptr, "mkdtemp must succeed");
    std::filesystem::path sandboxPath(sandbox);

    {
        StateEngine engine(sandboxPath);
        bool ok = engine.initialize(true);
        TEST_ASSERT(ok, "StateEngine initialize must succeed");

        struct stat st{};
        int rc = ::stat(engine.getStateDirectory().c_str(), &st);
        TEST_ASSERT_EQ(rc, 0, "State directory must exist");
        TEST_ASSERT_EQ(static_cast<int>(st.st_mode & 0777), 0700, "State directory mode must be 0700");

        rc = ::stat(engine.getStateFilePath().c_str(), &st);
        TEST_ASSERT_EQ(rc, 0, "status.json must exist");
        TEST_ASSERT_EQ(static_cast<int>(st.st_mode & 0777), 0600, "status.json mode must be 0600");

        // Verify JSON contents
        std::ifstream ifs(engine.getStateFilePath());
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        TEST_ASSERT(content.find("\"schema_version\":1") != std::string::npos, "Schema version must be 1");
        TEST_ASSERT(content.find("\"connected\":true") != std::string::npos, "Connected must be true");
        TEST_ASSERT(content.find("\"device_name\":\"WH-1000XM3\"") != std::string::npos, "Device name must match");
        TEST_ASSERT(content.find("\"ambient_max_level\"") != std::string::npos, "Must contain ambient_max_level");
        TEST_ASSERT(content.find("\"surround\"") != std::string::npos, "Must contain surround");
        TEST_ASSERT(content.find("\"dsee_hx\"") != std::string::npos, "Must contain dsee_hx");
        TEST_ASSERT(content.find("\"ambient_sound_level\"") != std::string::npos, "Must contain ambient_sound_level");
        TEST_ASSERT(content.find("\"ambient_level\"") != std::string::npos, "Must contain ambient_level");
        TEST_ASSERT(content.find("\"battery_charging\"") != std::string::npos, "Must contain battery_charging");
        TEST_ASSERT(content.find("\"charging\"") != std::string::npos, "Must contain charging");

        // Test state mutation
        engine.setNoiseMode("ambient");
        TEST_ASSERT(engine.setAmbientLevel(14), "Ambient step 14 must be accepted");
        engine.setEqPreset("vocal");

        // The level is the mode on this headset, so out-of-range must be refused
        // rather than silently clamped into a different mode.
        TEST_ASSERT(!engine.setAmbientLevel(20), "Ambient step above the reported max must be refused");
        TEST_ASSERT(!engine.setAmbientLevel(-1), "Negative ambient step must be refused");
        TEST_ASSERT(engine.setAmbientLevel(14), "Refused writes must leave the level usable");

        std::string jsonNow = engine.getStatusJson();
        TEST_ASSERT(jsonNow.find("\"noise_mode\":\"ambient\"") != std::string::npos, "Noise mode updated");
        TEST_ASSERT(jsonNow.find("\"ambient_sound_level\":14") != std::string::npos, "Ambient level updated");
        TEST_ASSERT(jsonNow.find("\"eq_preset\":\"vocal\"") != std::string::npos, "EQ preset updated");
        TEST_ASSERT(jsonNow.find('\n') == std::string::npos, "getStatusJson must not contain newlines");

        // Test concurrent readers (50 iterations)
        std::atomic<bool> writerDone{false};
        std::atomic<int> tornReads{0};

        std::thread reader([&]() {
            while (!writerDone.load()) {
                std::ifstream rfs(engine.getStateFilePath());
                std::string s((std::istreambuf_iterator<char>(rfs)), std::istreambuf_iterator<char>());
                if (!s.empty()) {
                    if (s.front() != '{' || s.back() != '\n') {
                        tornReads++;
                    }
                }
            }
        });

        for (int i = 0; i < 50; ++i) {
            engine.setAmbientLevel(i % 20);
        }
        writerDone = true;
        reader.join();
        TEST_ASSERT_EQ(tornReads.load(), 0, "No torn reads under concurrent atomic writes");

        // XM3-specific setters must validate their vocabulary.
        TEST_ASSERT(engine.setSurround("concert"), "Known surround preset must be accepted");
        TEST_ASSERT(!engine.setSurround("stadium"), "Unknown surround preset must be refused");
        TEST_ASSERT(engine.setSoundPosition("rear-left"), "Known sound position must be accepted");
        TEST_ASSERT(!engine.setSoundPosition("above"), "Unknown sound position must be refused");
        TEST_ASSERT(engine.setAutoPowerOff("30min"), "Known auto power off value must be accepted");
        TEST_ASSERT(!engine.setAutoPowerOff("7min"), "Unknown auto power off value must be refused");
        TEST_ASSERT(engine.setConnectionMode("stable"), "Known connection mode must be accepted");
        TEST_ASSERT(!engine.setConnectionMode("fast"), "Unknown connection mode must be refused");

        // A headset reporting a larger range must widen what the engine accepts.
        engine.setAmbientMaxLevel(20);
        TEST_ASSERT(engine.setAmbientLevel(20), "Level 20 must be accepted once the headset reports it");
        engine.setAmbientMaxLevel(19);

        // Test disconnect
        engine.setConnected(false);
        std::ifstream dis_fs(engine.getStateFilePath());
        std::string disContent((std::istreambuf_iterator<char>(dis_fs)), std::istreambuf_iterator<char>());
        TEST_ASSERT(disContent.find("\"connected\":false") != std::string::npos, "Disconnected payload persisted");
        TEST_ASSERT(std::filesystem::exists(engine.getStateFilePath()), "File remains on disconnect");

        // Test cleanup on shutdown
        engine.cleanup();
        TEST_ASSERT(!std::filesystem::exists(engine.getStateFilePath()), "File unlinked on clean shutdown");
    }

    std::error_code ec;
    std::filesystem::remove_all(sandboxPath, ec);

    TEST_PASS("StateEngine");
}

// ---------------------------------------------------------------------------
// 11. IpcServer Wire Protocol & Socket Tests
// ---------------------------------------------------------------------------
void testIpcServer() {
    TEST_CASE("IpcServer");

    IpcServer server;

    // Direct command execution checks
    TEST_ASSERT_EQ(server.handleCommandLine("noise anc"), "OK\n", "noise anc valid");
    TEST_ASSERT_EQ(server.handleCommandLine("noise wind"), "OK\n", "noise wind valid");
    TEST_ASSERT(server.handleCommandLine("noise invalid").rfind("ERR invalid mode", 0) == 0, "invalid noise rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("ambient-level 16"), "OK\n", "ambient-level 16 valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ambient-level 0"), "OK\n", "ambient-level 0 valid");
    TEST_ASSERT_EQ(server.handleCommandLine("ambient-level 19"), "OK\n", "ambient-level 19 valid");
    TEST_ASSERT(server.handleCommandLine("ambient-level 20").rfind("ERR ambient level out of range", 0) == 0, "ambient level above the XM3 max rejected");
    TEST_ASSERT(server.handleCommandLine("ambient-level 25").rfind("ERR ambient level out of range", 0) == 0, "out of range ambient level rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("noise ambient 12"), "OK\n", "noise ambient with an explicit level valid");
    TEST_ASSERT(server.handleCommandLine("noise ambient 1").rfind("ERR ambient level out of range", 0) == 0, "noise ambient below step 2 rejected");
    TEST_ASSERT(server.handleCommandLine("ambient-level abc").rfind("ERR invalid level", 0) == 0, "non-integer ambient level rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("eq vocal"), "OK\n", "eq vocal valid");
    TEST_ASSERT(server.handleCommandLine("eq invalid").rfind("ERR unknown eq preset", 0) == 0, "unknown eq preset rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("eq custom 1 2 3 4 5 6"), "OK\n", "eq custom valid");
    TEST_ASSERT(server.handleCommandLine("eq custom 1 2 3").rfind("ERR custom eq requires", 0) == 0, "short custom eq rejected");
    TEST_ASSERT(server.handleCommandLine("eq custom 1 2 3 4 15 0").rfind("ERR custom eq band out of range", 0) == 0, "out of range custom eq band rejected");
    TEST_ASSERT(server.handleCommandLine("eq custom 1 2 3 4 5 15").rfind("ERR clear bass out of range", 0) == 0, "out of range clear bass rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("eq user1"), "OK\n", "eq user1 valid");
    TEST_ASSERT_EQ(server.handleCommandLine("voice-focus on"), "OK\n", "voice-focus on valid");
    TEST_ASSERT_EQ(server.handleCommandLine("dsee off"), "OK\n", "dsee off valid");
    TEST_ASSERT(server.handleCommandLine("ear-detect off").rfind("ERR unknown command", 0) == 0, "the XM3 has no wearing sensor");
    TEST_ASSERT_EQ(server.handleCommandLine("surround club"), "OK\n", "surround club valid");
    TEST_ASSERT(server.handleCommandLine("surround stadium").rfind("ERR unknown surround preset", 0) == 0, "unknown surround rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("sound-position front-left"), "OK\n", "sound-position front-left valid");
    TEST_ASSERT(server.handleCommandLine("sound-position above").rfind("ERR unknown sound position", 0) == 0, "unknown sound position rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("auto-power-off 180min"), "OK\n", "auto-power-off 180min valid");
    TEST_ASSERT(server.handleCommandLine("auto-power-off on-remove").rfind("ERR unknown auto power off", 0) == 0, "on-remove needs a wearing sensor the XM3 lacks");
    TEST_ASSERT(server.handleCommandLine("auto-power-off 7min").rfind("ERR unknown auto power off", 0) == 0, "unknown auto power off rejected");
    TEST_ASSERT_EQ(server.handleCommandLine("connection stable"), "OK\n", "connection stable valid");
    TEST_ASSERT(server.handleCommandLine("connection fast").rfind("ERR unknown connection mode", 0) == 0, "unknown connection mode rejected");
    // Commands that only exist on the XM4/XM5 must not silently succeed here.
    TEST_ASSERT(server.handleCommandLine("speak-to-chat on").rfind("ERR unknown command", 0) == 0, "speak-to-chat is not an XM3 feature");
    TEST_ASSERT(server.handleCommandLine("multipoint on").rfind("ERR unknown command", 0) == 0, "multipoint is not an XM3 feature");
    TEST_ASSERT(server.handleCommandLine("").rfind("ERR empty command", 0) == 0, "empty command rejected");
    TEST_ASSERT(server.handleCommandLine("unknown_verb").rfind("ERR unknown command", 0) == 0, "unknown verb rejected");

    // Socket Functional Test
    char tmpl[] = "/tmp/test_omasonyxm3_ipc_XXXXXX";
    char* sandbox = ::mkdtemp(tmpl);
    TEST_ASSERT(sandbox != nullptr, "mkdtemp for socket test must succeed");
    std::string sockPath = std::string(sandbox) + "/test.sock";

    {
        IpcServer sockServer(sockPath);
        bool started = sockServer.start();
        TEST_ASSERT(started, "IpcServer start must succeed");

        struct stat st{};
        int rc = ::stat(sockPath.c_str(), &st);
        TEST_ASSERT_EQ(rc, 0, "Socket file must exist");
        TEST_ASSERT_EQ(static_cast<int>(st.st_mode & 0777), 0700, "Socket file mode must be 0700");

        // Connect client
        int clientFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(clientFd >= 0, "Client socket creation must succeed");

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

        rc = ::connect(clientFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT_EQ(rc, 0, "Connect to socket must succeed");

        // Server accepts client
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 1UL, "Server must have 1 connected client");

        // Test 1: Full command
        std::string cmd1 = "noise ambient\n";
        ::send(clientFd, cmd1.data(), cmd1.size(), 0);
        sockServer.pollOnce(50);

        char buf[256]{};
        ssize_t n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
        TEST_ASSERT_EQ(n, 3L, "Response must be 3 bytes ('OK\\n')");
        TEST_ASSERT_EQ(std::string(buf), "OK\n", "Response string must be 'OK\\n'");

        // Test 2: Chunked partial delivery ("ambient-" then "level 10\n")
        std::string chunk1 = "ambient-";
        ::send(clientFd, chunk1.data(), chunk1.size(), 0);
        sockServer.pollOnce(10);

        std::string chunk2 = "level 10\n";
        ::send(clientFd, chunk2.data(), chunk2.size(), 0);
        sockServer.pollOnce(50);

        std::memset(buf, 0, sizeof(buf));
        n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
        TEST_ASSERT_EQ(n, 3L, "Response to chunked command must be 3 bytes");
        TEST_ASSERT_EQ(std::string(buf), "OK\n", "Response to chunked command must be 'OK\\n'");

        // Test 3: Pipelined multiple commands in single write
        std::string pipelined = "dsee on\nvoice-focus off\n";
        ::send(clientFd, pipelined.data(), pipelined.size(), 0);
        sockServer.pollOnce(50);

        std::memset(buf, 0, sizeof(buf));
        n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
        TEST_ASSERT_EQ(n, 6L, "Response to pipelined commands must be 6 bytes ('OK\\nOK\\n')");
        TEST_ASSERT_EQ(std::string(buf), "OK\nOK\n", "Pipelined responses must be 'OK\\nOK\\n'");

        ::close(clientFd);
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 0UL, "Client count must be 0 after disconnect");

        // Set callbacks for status queries
        IpcCallbacks cbs;
        protocol::HeadphoneState testState;
        testState.connected = true;
        testState.device_name = "WH-1000XM3";
        testState.noise_mode = "anc";
        testState.battery_level = 90;
        cbs.getStatusJson = [&testState]() {
            return testState.toJson();
        };
        sockServer.setCallbacks(cbs);

        // Test 4: Compact single-line status output over socket
        int clientFd2 = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(clientFd2 >= 0, "Client 2 socket creation must succeed");
        rc = ::connect(clientFd2, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT_EQ(rc, 0, "Connect client 2 must succeed");
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 1UL, "Server must have 1 client connected");

        std::string statusCmd = "status\n";
        ::send(clientFd2, statusCmd.data(), statusCmd.size(), 0);
        sockServer.pollOnce(50);

        char respBuf[4096]{};
        n = ::recv(clientFd2, respBuf, sizeof(respBuf) - 1, 0);
        TEST_ASSERT(n > 0, "Status response received from socket");
        std::string statusResp(respBuf, static_cast<size_t>(n));
        TEST_ASSERT(statusResp.back() == '\n', "Status response must terminate in newline");
        TEST_ASSERT_EQ(statusResp.find('\n'), statusResp.size() - 1, "Status response must have no embedded newlines before the final newline");
        TEST_ASSERT(statusResp.find("\"device_name\":\"WH-1000XM3\"") != std::string::npos, "Status response contains compact device_name");
        TEST_ASSERT(statusResp.find("\"noise_mode\":\"anc\"") != std::string::npos, "Status response contains compact noise_mode");

        ::close(clientFd2);
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 0UL, "Client count must be 0 after disconnect");

        // Test 5: Pipelined commands under abrupt client disconnect (UAF prevention check)
        int clientFd3 = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(clientFd3 >= 0, "Client 3 socket creation must succeed");
        rc = ::connect(clientFd3, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT_EQ(rc, 0, "Connect client 3 must succeed");
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 1UL, "Server must have 1 client connected");

        std::string pipeBurst = "status\nnoise anc\nstatus\nambient-level 5\nstatus\n";
        ::send(clientFd3, pipeBurst.data(), pipeBurst.size(), 0);
        ::shutdown(clientFd3, SHUT_RD);
        ::close(clientFd3);

        // Server processes pipelined read event on abruptly closed socket: must not crash or UAF
        sockServer.pollOnce(50);
        sockServer.pollOnce(50);
        TEST_ASSERT_EQ(sockServer.getClientCount(), 0UL, "Server must cleanly handle abrupt disconnect during pipelining without crash");

        sockServer.stop();
        rc = ::stat(sockPath.c_str(), &st);
        TEST_ASSERT_EQ(rc, -1, "Socket file must be unlinked after stop()");
    }

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    TEST_PASS("IpcServer");
}

// ---------------------------------------------------------------------------
// 12. Command queue: one frame in flight, sequence taken from ACKs only
//
// Both behaviours were found on real hardware, where every offline test had
// passed: the XM3 silently drops frames sent before the previous ACK, and
// taking the sequence number from its notifications made the next command
// time out before its retry landed.
// ---------------------------------------------------------------------------
void testCommandQueue() {
    TEST_CASE("CommandQueue");

    std::vector<UnpackedFrame> sent;
    CommandQueue q([&](const std::vector<uint8_t>& frame) {
        auto f = unpackFrame(frame);
        if (f) sent.push_back(*f);
    });

    // --- One in flight ------------------------------------------------------
    q.enqueue(serializeQueryBattery(), "a");
    q.enqueue(serializeQueryNoiseMode(), "b");
    q.enqueue(serializeQueryEq(), "c");
    TEST_ASSERT_EQ(sent.size(), static_cast<size_t>(1), "Only the first command may be sent before an ACK");
    TEST_ASSERT_EQ(static_cast<int>(sent[0].seq), 0, "First command uses sequence 0");
    TEST_ASSERT_EQ(q.queued(), static_cast<size_t>(2), "The other two wait");

    // --- The ACK releases the next, and names its sequence number -----------
    q.onFrame(PacketType::ACK, 1);
    TEST_ASSERT_EQ(sent.size(), static_cast<size_t>(2), "An ACK releases exactly one more command");
    TEST_ASSERT_EQ(static_cast<int>(sent[1].seq), 1, "The next command takes the ACK's sequence number");
    TEST_ASSERT_EQ(static_cast<int>(sent[1].payload[0]), 0x66, "Commands go out in the order queued");

    // --- Headset notifications must not move the sequence -------------------
    q.onFrame(PacketType::DATA_MDR, 1);   // reply to the query
    q.onFrame(PacketType::DATA_MDR, 0);   // an unrelated volume notification
    TEST_ASSERT_EQ(sent.size(), static_cast<size_t>(2), "DATA frames never release the next command");
    q.onFrame(PacketType::ACK, 0);
    TEST_ASSERT_EQ(static_cast<int>(sent[2].seq), 0, "Sequence follows the ACK, not the notifications before it");

    q.onFrame(PacketType::ACK, 1);
    TEST_ASSERT(!q.busy(), "Queue is idle once everything is ACKed");
    q.onFrame(PacketType::DATA_MDR, 0);
    TEST_ASSERT_EQ(static_cast<int>(q.nextSeq()), 1, "An idle queue keeps the last ACK's sequence despite notifications");

    // --- A lost ACK: retry with the sequence bit flipped ---------------------
    sent.clear();
    q.enqueue(serializeQueryCodec(), "retry");
    TEST_ASSERT_EQ(static_cast<int>(sent[0].seq), 1, "Sent with the expected sequence");
    q.tick(CommandQueue::Clock::now());
    TEST_ASSERT_EQ(sent.size(), static_cast<size_t>(1), "No retry before the ACK timeout");
    q.tick(CommandQueue::Clock::now() + std::chrono::seconds(2));
    TEST_ASSERT_EQ(sent.size(), static_cast<size_t>(2), "Retry once the ACK timeout passes");
    TEST_ASSERT_EQ(static_cast<int>(sent[1].seq), 0, "The retry flips the sequence bit");
    TEST_ASSERT(sent[1].payload == sent[0].payload, "The retry resends the same command");

    // --- Give up after the retry budget and move on -------------------------
    q.enqueue(serializeQueryEq(), "after-drop");
    for (int i = 0; i < 5; ++i) q.tick(CommandQueue::Clock::now() + std::chrono::seconds(2));
    TEST_ASSERT(q.busy(), "The queue moves on to the next command after dropping one");
    TEST_ASSERT_EQ(static_cast<int>(sent.back().payload[0]), 0x56, "The next command is the one queued behind it");

    // --- Reset on disconnect -------------------------------------------------
    q.reset();
    TEST_ASSERT(!q.busy() && q.queued() == 0, "reset() clears everything");
    TEST_ASSERT_EQ(static_cast<int>(q.nextSeq()), 0, "reset() restarts the sequence at 0");

    TEST_PASS("CommandQueue");
}

// ---------------------------------------------------------------------------
// Main Runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "========================================\n";
    std::cout << "  omarchy-sony-xm3: Protocol & Mock Tests\n";
    std::cout << "========================================\n";

    testChecksumCalculation();
    testEscapingAndUnescaping();
    testPacketFraming();
    testStreamFramer();
    testCommandSerializers();
    testQuerySerializers();
    testInboundStateParser();
    testStringAndEnumHelpers();
    testMockTransportAndBluetoothManager();
    testStateEngine();
    testIpcServer();
    testCommandQueue();

    std::cout << "========================================\n";
    std::cout << "Summary: " << (gTotalTests - gFailedTests) << "/" << gTotalTests
              << " test cases passed (" << gFailedTests << " failed)\n";
    std::cout << "========================================\n";

    return (gFailedTests == 0) ? 0 : 1;
}
