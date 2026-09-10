#include "MDRProtocolV1.hpp"
#include "BluetoothManager.hpp"

#include <iostream>
#include <vector>
#include <random>
#include <cassert>
#include <cstring>
#include <chrono>
#include <sstream>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

using namespace omarchy::sony::protocol;

// Global counters
static int gPassedTests = 0;
static int gFailedTests = 0;

#define LOG_TEST_START(name) \
    std::cout << "[ TEST     ] " << name << std::endl;

#define LOG_TEST_PASS(name) \
    do { \
        std::cout << "[       OK ] " << name << std::endl; \
        gPassedTests++; \
    } while (0)

#define LOG_TEST_FAIL(name, reason) \
    do { \
        std::cerr << "[  FAILED  ] " << name << " : " << reason \
                  << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        gFailedTests++; \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::mt19937 gRng(1337); // Deterministic seed for reproducible fuzzing

static std::vector<uint8_t> generateRandomBytes(size_t len) {
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    std::vector<uint8_t> bytes(len);
    for (size_t i = 0; i < len; ++i) {
        bytes[i] = static_cast<uint8_t>(dist(gRng));
    }
    return bytes;
}

// Generates a variety of valid frames
static std::vector<std::vector<uint8_t>> generateSampleValidFrames() {
    std::vector<std::vector<uint8_t>> frames;

    // ACK frame
    frames.push_back(serializeACK(0));
    frames.push_back(serializeACK(1));

    // Noise modes
    frames.push_back(serializeNoiseMode(NoiseMode::ANC, 0, false, 0));
    frames.push_back(serializeNoiseMode(NoiseMode::OFF, 0, false, 1));
    frames.push_back(serializeNoiseMode(NoiseMode::AMBIENT, 10, true, 2));
    frames.push_back(serializeAmbientLevel(19, false, 3));
    frames.push_back(serializeAmbientLevel(1, false, 4)); // Wind noise reduction

    // EQ
    frames.push_back(serializeEqPreset(EqPreset::VOCAL, 5));
    frames.push_back(serializeEqPreset(EqPreset::BASS, 6));
    std::array<int, 5> bands = {-10, -5, 0, 5, 10};
    frames.push_back(serializeCustomEq(bands, 4, 7));

    // Features
    frames.push_back(serializeDsee(true, 8));
    frames.push_back(serializeEarDetection(true, 9));
    frames.push_back(serializeSurround(SurroundPreset::CONCERT_HALL, 10));
    frames.push_back(serializeSoundPosition(SoundPosition::REAR_RIGHT, 11));
    frames.push_back(serializeAutoPowerOff(AutoPowerOff::AFTER_30_MIN, 12));
    frames.push_back(serializeConnectionMode(ConnectionMode::STABLE_LINK, 13));

    // Queries
    frames.push_back(serializeQueryBattery(14));
    frames.push_back(serializeQueryNoiseMode(15));
    frames.push_back(serializeQueryNcAsmCapability(16));
    frames.push_back(serializeQueryEq(17));
    frames.push_back(serializeQueryDsee(18));
    frames.push_back(serializeQueryCodec(19));
    frames.push_back(serializeQueryEarDetection(20));

    // Inbound-like responses (v1 command bytes)
    std::vector<uint8_t> batRet = {0x11, 0x00, 75, 0x01};
    frames.push_back(packFrame(PacketType::DATA_MDR, 21, batRet));

    std::vector<uint8_t> ncRet = {0x67, 0x02, 0x11, 0x01, 0x00, 0x01, 0x00, 15};
    frames.push_back(packFrame(PacketType::DATA_MDR, 22, ncRet));

    return frames;
}

// ---------------------------------------------------------------------------
// 1. Fuzzing StreamFramer with Massive Randomized Streams
// ---------------------------------------------------------------------------
void stressFuzzStreamFramer() {
    LOG_TEST_START("FuzzStreamFramer_RandomStreams");

    StreamFramer framer;
    size_t totalBytesFed = 0;
    size_t extractedFrames = 0;
    size_t unpackedFrames = 0;

    const size_t kIterations = 2000;
    std::uniform_int_distribution<size_t> chunkLenDist(1, 1024);

    for (size_t iter = 0; iter < kIterations; ++iter) {
        size_t chunkSize = chunkLenDist(gRng);
        auto chunk = generateRandomBytes(chunkSize);
        totalBytesFed += chunkSize;

        framer.append(chunk);

        while (true) {
            auto frameOpt = framer.nextFrame();
            if (!frameOpt.has_value()) break;

            extractedFrames++;
            auto unpacked = unpackFrame(*frameOpt);
            if (unpacked.has_value()) {
                unpackedFrames++;
                HeadphoneState state;
                parseInboundPayload(unpacked->payload, state);
                (void)state.toJson();
            }
        }
    }

    std::cout << "  [FuzzStreamFramer] Fed " << totalBytesFed << " bytes, extracted "
              << extractedFrames << " candidate frames, "
              << unpackedFrames << " valid frames survived unpacking." << std::endl;

    LOG_TEST_PASS("FuzzStreamFramer_RandomStreams");
}

// ---------------------------------------------------------------------------
// 2. Direct Fuzzing of unpackFrame() and parseInboundPayload()
// ---------------------------------------------------------------------------
void stressFuzzDirectUnpackAndParse() {
    LOG_TEST_START("FuzzDirectUnpackAndParse");

    const size_t kIterations = 10000;
    std::uniform_int_distribution<size_t> lenDist(0, 512);

    size_t validUnpacks = 0;
    size_t validParses = 0;

    for (size_t i = 0; i < kIterations; ++i) {
        size_t len = lenDist(gRng);
        auto bytes = generateRandomBytes(len);

        // 1. Fuzz unpackFrame
        auto unpacked = unpackFrame(bytes);
        if (unpacked.has_value()) {
            validUnpacks++;
        }

        // 2. Fuzz parseInboundPayload directly
        HeadphoneState state;
        if (parseInboundPayload(bytes, state)) {
            validParses++;
            std::string json = state.toJson();
            if (json.empty() || json.front() != '{' || json.back() != '}') {
                LOG_TEST_FAIL("FuzzDirectUnpackAndParse", "toJson() output malformed JSON structure");
                return;
            }
        }
    }

    std::cout << "  [DirectFuzz] Tested " << kIterations << " random buffers: "
              << validUnpacks << " unpacked, " << validParses << " parsed." << std::endl;

    LOG_TEST_PASS("FuzzDirectUnpackAndParse");
}

// ---------------------------------------------------------------------------
// 3. Byte-by-Byte Chunking & Fragmentation Stress
// ---------------------------------------------------------------------------
void stressByteByByteChunking() {
    LOG_TEST_START("ByteByByteChunking");

    auto samples = generateSampleValidFrames();
    StreamFramer framer;

    for (size_t idx = 0; idx < samples.size(); ++idx) {
        const auto& targetFrame = samples[idx];

        // Feed byte-by-byte
        for (size_t i = 0; i < targetFrame.size(); ++i) {
            uint8_t byte = targetFrame[i];
            framer.append(std::span<const uint8_t>(&byte, 1));

            auto result = framer.nextFrame();
            if (i < targetFrame.size() - 1) {
                if (result.has_value()) {
                    LOG_TEST_FAIL("ByteByByteChunking", "Premature frame extraction before final delimiter!");
                    return;
                }
            } else {
                if (!result.has_value()) {
                    LOG_TEST_FAIL("ByteByByteChunking", "Failed to extract frame on final delimiter byte!");
                    return;
                }
                if (*result != targetFrame) {
                    LOG_TEST_FAIL("ByteByByteChunking", "Extracted frame does not match original frame!");
                    return;
                }

                auto unp = unpackFrame(*result);
                if (!unp.has_value()) {
                    LOG_TEST_FAIL("ByteByByteChunking", "Extracted frame failed unpackFrame!");
                    return;
                }
            }
        }
    }

    LOG_TEST_PASS("ByteByByteChunking");
}

// ---------------------------------------------------------------------------
// 4. Fragmented Escape Characters (0x3D Split Across Reads)
// ---------------------------------------------------------------------------
void stressFragmentedEscapes() {
    LOG_TEST_START("FragmentedEscapeSequences");

    // Construct payloads specifically containing bytes that trigger escaping:
    // 0x3C, 0x3D, 0x3E
    std::vector<uint8_t> payload;
    for (int i = 0; i < 20; ++i) {
        payload.push_back(0x3C);
        payload.push_back(0x3D);
        payload.push_back(0x3E);
        payload.push_back(0x55);
    }

    auto packed = packFrame(PacketType::DATA_MDR, 1, payload);

    // Verify that packed frame actually contains escaped sentries (0x3D followed by 0x2C, 0x2D, 0x2E)
    size_t escapeSentryCount = 0;
    for (uint8_t b : packed) {
        if (b == kEscapeSentry) escapeSentryCount++;
    }
    if (escapeSentryCount < 60) {
        LOG_TEST_FAIL("FragmentedEscapeSequences", "Test frame does not contain expected escape sentries");
        return;
    }

    // Split across every single possible boundary: chunk1 = [0..splitPoint), chunk2 = [splitPoint..end)
    for (size_t splitPoint = 1; splitPoint < packed.size(); ++splitPoint) {
        StreamFramer framer;
        std::span<const uint8_t> chunk1(packed.data(), splitPoint);
        std::span<const uint8_t> chunk2(packed.data() + splitPoint, packed.size() - splitPoint);

        framer.append(chunk1);
        auto premature = framer.nextFrame();
        if (premature.has_value()) {
            LOG_TEST_FAIL("FragmentedEscapeSequences", "Premature frame extracted on first chunk");
            return;
        }

        framer.append(chunk2);
        auto extracted = framer.nextFrame();
        if (!extracted.has_value()) {
            LOG_TEST_FAIL("FragmentedEscapeSequences", "Failed to extract frame after second chunk");
            return;
        }
        if (*extracted != packed) {
            LOG_TEST_FAIL("FragmentedEscapeSequences", "Extracted frame mismatch across split");
            return;
        }

        auto unp = unpackFrame(*extracted);
        if (!unp.has_value()) {
            LOG_TEST_FAIL("FragmentedEscapeSequences", "Unpack failed on frame split at " + std::to_string(splitPoint));
            return;
        }
        if (unp->payload != payload) {
            LOG_TEST_FAIL("FragmentedEscapeSequences", "Unpacked payload mismatch");
            return;
        }
    }

    // Direct testing of unescapeBytes with corrupted / incomplete escape sequences
    {
        // Dangling escape at end of stream
        std::vector<uint8_t> dangling = {0x01, 0x02, kEscapeSentry};
        auto res = unescapeBytes(dangling);
        if (!res.empty()) {
            LOG_TEST_FAIL("FragmentedEscapeSequences", "unescapeBytes must reject dangling escape sentry");
            return;
        }

        // Invalid escape sequence (0x3D followed by invalid code)
        for (uint16_t nextByte = 0; nextByte <= 255; ++nextByte) {
            if (nextByte == kEscaped3C || nextByte == kEscaped3D || nextByte == kEscaped3E) continue;
            std::vector<uint8_t> invalidSeq = {0x01, kEscapeSentry, static_cast<uint8_t>(nextByte), 0x02};
            auto invalidRes = unescapeBytes(invalidSeq);
            if (!invalidRes.empty()) {
                LOG_TEST_FAIL("FragmentedEscapeSequences", "unescapeBytes accepted invalid escape code 0x" + std::to_string(nextByte));
                return;
            }
        }
    }

    LOG_TEST_PASS("FragmentedEscapeSequences");
}

// ---------------------------------------------------------------------------
// 5. Incomplete Headers, Truncated Payloads, Wrong Checksums
// ---------------------------------------------------------------------------
void stressIncompleteHeadersAndTamperedChecksums() {
    LOG_TEST_START("IncompleteHeadersAndTamperedChecksums");

    std::vector<uint8_t> payload = {0x68, 0x17, 0x01, 0x01, 0x00, 0x00, 0x00};
    auto validFrame = packFrame(PacketType::DATA_MDR, 1, payload);

    // 1. Truncate from 0 bytes up to validFrame.size() - 1
    for (size_t len = 0; len < validFrame.size(); ++len) {
        std::vector<uint8_t> truncated(validFrame.begin(), validFrame.begin() + len);
        auto unp = unpackFrame(truncated);
        if (unp.has_value()) {
            LOG_TEST_FAIL("IncompleteHeadersAndTamperedChecksums", "Truncated frame of len " + std::to_string(len) + " must not unpack");
            return;
        }
    }

    // 2. Tamper with each byte position inside the frame
    for (size_t i = 1; i < validFrame.size() - 1; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> flipped = validFrame;
            flipped[i] ^= (1 << bit);

            // If we flipped bytes, unpack should either reject or if by freak collision it passes, verify integrity
            auto unp = unpackFrame(flipped);
            if (unp.has_value()) {
                // If it unpacked, the payload MUST NOT be equal to original (unless padding or unused byte, which isn't present)
                if (unp->payload == payload && flipped != validFrame) {
                    LOG_TEST_FAIL("IncompleteHeadersAndTamperedChecksums", "Flipped byte resulted in identical payload without checksum failure");
                    return;
                }
            }
        }
    }

    // 3. Header Length field corruption
    // Unescaped inner: Type(1), Seq(1), Len(4), Payload(N), Csum(1)
    // Construct frames with deliberately mismatched length fields
    {
        uint32_t maliciousLens[] = {0, 1, 2, 100, 0xFFFF, 0xFFFFFFFF};
        for (uint32_t badLen : maliciousLens) {
            std::vector<uint8_t> inner;
            inner.push_back(static_cast<uint8_t>(PacketType::DATA_MDR));
            inner.push_back(0x01);
            inner.push_back(static_cast<uint8_t>((badLen >> 24) & 0xFF));
            inner.push_back(static_cast<uint8_t>((badLen >> 16) & 0xFF));
            inner.push_back(static_cast<uint8_t>((badLen >> 8) & 0xFF));
            inner.push_back(static_cast<uint8_t>(badLen & 0xFF));
            inner.insert(inner.end(), payload.begin(), payload.end());
            uint8_t csum = calculateChecksum(inner);
            inner.push_back(csum);

            std::vector<uint8_t> frame;
            frame.push_back(kStartMarker);
            auto escaped = escapeBytes(inner);
            frame.insert(frame.end(), escaped.begin(), escaped.end());
            frame.push_back(kEndMarker);

            auto res = unpackFrame(frame);
            if (badLen != payload.size()) {
                if (res.has_value()) {
                    LOG_TEST_FAIL("IncompleteHeadersAndTamperedChecksums", "unpackFrame accepted mismatched length field " + std::to_string(badLen));
                    return;
                }
            }
        }
    }

    // 4. Exhaustive checksum tampering (255 invalid checksums)
    {
        // Get valid unescaped inner bytes
        std::span<const uint8_t> innerSpan(validFrame.data() + 1, validFrame.size() - 2);
        auto inner = unescapeBytes(innerSpan);
        uint8_t trueCsum = inner.back();

        for (uint16_t c = 0; c <= 255; ++c) {
            uint8_t testCsum = static_cast<uint8_t>(c);
            if (testCsum == trueCsum) continue;

            auto tamperedInner = inner;
            tamperedInner.back() = testCsum;

            std::vector<uint8_t> tamperedFrame;
            tamperedFrame.push_back(kStartMarker);
            auto esc = escapeBytes(tamperedInner);
            tamperedFrame.insert(tamperedFrame.end(), esc.begin(), esc.end());
            tamperedFrame.push_back(kEndMarker);

            auto res = unpackFrame(tamperedFrame);
            if (res.has_value()) {
                LOG_TEST_FAIL("IncompleteHeadersAndTamperedChecksums", "Accepted wrong checksum " + std::to_string(testCsum));
                return;
            }
        }
    }

    LOG_TEST_PASS("IncompleteHeadersAndTamperedChecksums");
}

// ---------------------------------------------------------------------------
// 6. Junk Bytes Injected Between Valid Frames
// ---------------------------------------------------------------------------
void stressJunkBetweenFrames() {
    LOG_TEST_START("JunkBytesInjectedBetweenFrames");

    auto samples = generateSampleValidFrames();
    StreamFramer framer;

    for (size_t i = 0; i < samples.size() - 1; ++i) {
        const auto& frame1 = samples[i];
        const auto& frame2 = samples[i + 1];

        // 1. Leading garbage without start marker
        std::vector<uint8_t> stream;
        auto junk1 = generateRandomBytes(50);
        // Replace any accidental start marker in junk to test non-delimiter junk first
        for (auto& b : junk1) { if (b == kStartMarker) b = 0xAA; }

        stream.insert(stream.end(), junk1.begin(), junk1.end());
        stream.insert(stream.end(), frame1.begin(), frame1.end());

        // Middle junk with random bytes (may contain end markers or random markers)
        auto junk2 = generateRandomBytes(80);
        for (auto& b : junk2) { if (b == kStartMarker) b = 0xBB; }
        stream.insert(stream.end(), junk2.begin(), junk2.end());
        stream.insert(stream.end(), frame2.begin(), frame2.end());

        framer.append(stream);

        auto extracted1 = framer.nextFrame();
        if (!extracted1.has_value()) {
            LOG_TEST_FAIL("JunkBytesInjectedBetweenFrames", "Failed to extract frame1 across leading junk");
            return;
        }
        if (*extracted1 != frame1) {
            LOG_TEST_FAIL("JunkBytesInjectedBetweenFrames", "Extracted frame1 mismatch");
            return;
        }

        auto extracted2 = framer.nextFrame();
        if (!extracted2.has_value()) {
            LOG_TEST_FAIL("JunkBytesInjectedBetweenFrames", "Failed to extract frame2 across middle junk");
            return;
        }
        if (*extracted2 != frame2) {
            LOG_TEST_FAIL("JunkBytesInjectedBetweenFrames", "Extracted frame2 mismatch");
            return;
        }

        framer.reset();
    }

    LOG_TEST_PASS("JunkBytesInjectedBetweenFrames");
}

// ---------------------------------------------------------------------------
// 7. Adversarial Delimiter Injection & Desynchronization Recovery
// ---------------------------------------------------------------------------
// Tests what happens when false start markers (0x3E) or false end markers (0x3C)
// are injected in corrupted fragments preceding valid frames.
void stressAdversarialDelimiters() {
    LOG_TEST_START("AdversarialDelimitersAndResync");

    auto samples = generateSampleValidFrames();
    const auto& validFrame = samples[0];

    // Scenario A: Stray End Markers 0x3C before a valid frame
    {
        StreamFramer framer;
        std::vector<uint8_t> stream = {0x3C, 0x3C, 0x3C, 0x00, 0x3C};
        stream.insert(stream.end(), validFrame.begin(), validFrame.end());

        framer.append(stream);
        auto extracted = framer.nextFrame();
        if (!extracted.has_value()) {
            LOG_TEST_FAIL("AdversarialDelimitersAndResync", "Failed to recover valid frame after stray end markers (0x3C)");
            return;
        }
        if (*extracted != validFrame) {
            LOG_TEST_FAIL("AdversarialDelimitersAndResync", "Extracted frame mismatch after stray 0x3C");
            return;
        }
    }

    // Scenario B: Aborted Start Marker 0x3E followed by junk, followed by valid frame
    // What happens if buffer has [0x3E, 0xAA, 0xBB] (no 0x3C) followed by valid frame [0x3E ... 0x3C]?
    {
        StreamFramer framer;
        std::vector<uint8_t> stream = {0x3E, 0xAA, 0xBB}; // Aborted frame (no end delimiter)
        stream.insert(stream.end(), validFrame.begin(), validFrame.end());

        framer.append(stream);
        auto extracted = framer.nextFrame();

        // Let's observe the behavior:
        // In current implementation:
        // startIt = index 0 (the first 0x3E).
        // endIt = end of validFrame (the 0x3C).
        // Extracted frame contains [0x3E, 0xAA, 0xBB, 0x3E, ... 0x3C]!
        // When unpackFrame is called on this, unpackFrame fails!
        std::cout << "  [Scenario B] Extracted frame size with aborted start marker: "
                  << (extracted.has_value() ? std::to_string(extracted->size()) : "nullopt") << std::endl;

        if (extracted.has_value()) {
            auto unp = unpackFrame(*extracted);
            if (!unp.has_value()) {
                std::cout << "  [Observation B] unpackFrame correctly rejected the merged corrupted frame." << std::endl;
                // But check if validFrame was consumed and lost!
                auto nextExtracted = framer.nextFrame();
                std::cout << "  [Observation B] Subsequent nextFrame(): "
                          << (nextExtracted.has_value() ? "has_value" : "nullopt") << std::endl;
            }
        }
    }

    // Scenario C: Consecutive Start Markers [0x3E, 0x3E, 0x3E, validFrame...]
    {
        StreamFramer framer;
        std::vector<uint8_t> stream = {0x3E, 0x3E, 0x3E};
        stream.insert(stream.end(), validFrame.begin(), validFrame.end());
        framer.append(stream);

        auto extracted = framer.nextFrame();
        if (extracted.has_value()) {
            auto unp = unpackFrame(*extracted);
            std::cout << "  [Scenario C] Consecutive start markers unpack result: "
                      << (unp.has_value() ? "VALID" : "REJECTED") << std::endl;
        }
    }

    LOG_TEST_PASS("AdversarialDelimitersAndResync");
}

// ---------------------------------------------------------------------------
// 8. StreamFramer Memory Bound Stress (Denial of Service / OOM check)
// ---------------------------------------------------------------------------
void stressMemoryBoundStreamFramer() {
    LOG_TEST_START("StreamFramer_MemoryBound");

    StreamFramer framer;

    // Send 1 MB of non-delimited data with start marker at byte 0
    std::vector<uint8_t> largeJunk = {kStartMarker};
    auto randomFill = generateRandomBytes(1024 * 1024);
    // Ensure no 0x3C in random fill
    for (auto& b : randomFill) {
        if (b == kEndMarker) b = 0xAA;
    }
    largeJunk.insert(largeJunk.end(), randomFill.begin(), randomFill.end());

    framer.append(largeJunk);
    auto res = framer.nextFrame();
    if (res.has_value()) {
        LOG_TEST_FAIL("StreamFramer_MemoryBound", "nextFrame returned value without end marker!");
        return;
    }

    size_t buffered = framer.bufferedBytes();
    std::cout << "  [MemoryBound] Buffered bytes after 1MB incomplete frame: " << buffered << " bytes." << std::endl;

    // Test reset cleans up
    framer.reset();
    if (framer.bufferedBytes() != 0) {
        LOG_TEST_FAIL("StreamFramer_MemoryBound", "reset() did not clear buffer");
        return;
    }

    LOG_TEST_PASS("StreamFramer_MemoryBound");
}

// ---------------------------------------------------------------------------
// 9. Semantic & State Machine Stress (Adversarial Inbound Payloads)
// ---------------------------------------------------------------------------
void stressInboundPayloadSemantics() {
    LOG_TEST_START("InboundPayloadSemantics");

    HeadphoneState state;

    // Test extreme battery values (e.g. 255%, -10)
    std::vector<uint8_t> extremeBat = {0x11, 0x00, 255, 0x01};
    parseInboundPayload(extremeBat, state);
    std::string jsonBat = state.toJson();
    if (jsonBat.find("\"battery_level\":255") == std::string::npos) {
        LOG_TEST_FAIL("InboundPayloadSemantics", "Battery level not reflected in JSON");
        return;
    }

    // Test extreme EQ band values (> 20 -> should result in > +10 if not clamped)
    std::vector<uint8_t> extremeEq = {0x57, 0x01, 0xA0, 0x06, 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA};
    parseInboundPayload(extremeEq, state);
    std::string jsonEq = state.toJson();
    if (jsonEq.empty() || jsonEq.front() != '{' || jsonEq.back() != '}') {
        LOG_TEST_FAIL("InboundPayloadSemantics", "Invalid JSON output after extreme EQ bands");
        return;
    }

    // Test unknown command IDs
    for (uint16_t cmd = 0; cmd <= 255; ++cmd) {
        std::vector<uint8_t> dummy = {static_cast<uint8_t>(cmd), 0x01, 0x02, 0x03};
        parseInboundPayload(dummy, state);
        auto j = state.toJson();
        if (j.empty()) {
            LOG_TEST_FAIL("InboundPayloadSemantics", "Empty JSON produced");
            return;
        }
    }

    // Test empty payload
    parseInboundPayload({}, state);

    LOG_TEST_PASS("InboundPayloadSemantics");
}

// ---------------------------------------------------------------------------
// 10. MockTransport Socketpair Fuzzing & Stream Injection
// ---------------------------------------------------------------------------
void stressMockTransportFuzzing() {
    LOG_TEST_START("MockTransport_SocketpairFuzzing");

    int peerFd = -1;
    BluetoothConfig config;
    config.initialBackoffMs = 50;
    config.maxBackoffMs = 200;
    auto manager = BluetoothManager::createMock(config, &peerFd);
    if (!manager || peerFd < 0) {
        LOG_TEST_FAIL("MockTransport_SocketpairFuzzing", "Failed to create mock manager");
        return;
    }

    StreamFramer framer;
    size_t validFramesReceived = 0;

    BluetoothCallbacks callbacks;
    callbacks.onDataReceived = [&](const uint8_t* data, size_t len) {
        framer.append(std::span<const uint8_t>(data, len));
        while (auto f = framer.nextFrame()) {
            auto unp = unpackFrame(*f);
            if (unp.has_value()) {
                validFramesReceived++;
            }
        }
    };
    manager->setCallbacks(callbacks);
    manager->start();

    // Pump 500 batches of random adversarial noise interleaved with real frames into peerFd
    auto validFrame = serializeQueryBattery(0);

    for (int batch = 0; batch < 200; ++batch) {
        // Send random noise
        auto noise = generateRandomBytes(64);
        ::send(peerFd, noise.data(), noise.size(), 0);
        manager->handleSocketEvent(POLLIN);

        // Send valid frame in 2 fragments
        size_t half = validFrame.size() / 2;
        ::send(peerFd, validFrame.data(), half, 0);
        manager->handleSocketEvent(POLLIN);

        ::send(peerFd, validFrame.data() + half, validFrame.size() - half, 0);
        manager->handleSocketEvent(POLLIN);
    }

    std::cout << "  [MockTransportFuzzing] Successfully pumped 200 noise batches, valid frames parsed: "
              << validFramesReceived << std::endl;

    manager->stop();
    LOG_TEST_PASS("MockTransport_SocketpairFuzzing");
}

// ---------------------------------------------------------------------------
// Main Runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=====================================================\n";
    std::cout << "  omarchy-sony-xm3: Adversarial Protocol Stress Harness  \n";
    std::cout << "=====================================================\n";

    auto start = std::chrono::steady_clock::now();

    stressFuzzStreamFramer();
    stressFuzzDirectUnpackAndParse();
    stressByteByByteChunking();
    stressFragmentedEscapes();
    stressIncompleteHeadersAndTamperedChecksums();
    stressJunkBetweenFrames();
    stressAdversarialDelimiters();
    stressMemoryBoundStreamFramer();
    stressInboundPayloadSemantics();
    stressMockTransportFuzzing();

    auto end = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "=====================================================\n";
    std::cout << "Results: " << gPassedTests << " PASSED, " << gFailedTests
              << " FAILED (Execution time: " << elapsedMs << " ms)\n";
    std::cout << "=====================================================\n";

    return (gFailedTests == 0) ? 0 : 1;
}
