#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <optional>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/signalfd.h>

#include "BluetoothManager.hpp"
#include "MDRProtocolV1.hpp"
#include "StateEngine.hpp"
#include "IpcServer.hpp"
#include "CommandQueue.hpp"

namespace {

using namespace omarchy::sony;
using namespace omarchy::sony::protocol;

constexpr const char* DAEMON_VERSION = "0.1.0";
constexpr const char* DEFAULT_DEVICE_NAME = "WH-1000XM3";

struct DaemonOptions {
    bool showHelp{false};
    bool showVersion{false};
    bool mockMode{false};
    std::string preferredMac;
    std::string stateDir;
    std::string runtimeDir;
};

void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [OPTIONS]\n\n"
              << "Headless background daemon managing Sony WH-1000XM3 headphones on Linux.\n\n"
              << "Options:\n"
              << "  -h, --help               Display this help message and exit\n"
              << "  -v, --version            Display version information and exit\n"
              << "  -m, --mock               Run in mock simulation mode (completely offline)\n"
              << "  -d, --device <MAC>       Target specific Bluetooth MAC address\n"
              << "      --state-dir <DIR>    Override directory for status.json\n"
              << "      --runtime-dir <DIR>  Override directory for IPC socket\n\n";
}

void printVersion() {
    std::cout << "sony-xm3-daemon " << DAEMON_VERSION << "\n";
}

std::optional<DaemonOptions> parseCommandLine(int argc, char* argv[]) {
    DaemonOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
            return opts;
        } else if (arg == "-v" || arg == "--version") {
            opts.showVersion = true;
            return opts;
        } else if (arg == "-m" || arg == "--mock") {
            opts.mockMode = true;
        } else if (arg == "-d" || arg == "--device") {
            if (i + 1 < argc) {
                opts.preferredMac = argv[++i];
            } else {
                std::cerr << "Error: --device requires a MAC address argument\n";
                return std::nullopt;
            }
        } else if (arg == "--state-dir") {
            if (i + 1 < argc) {
                opts.stateDir = argv[++i];
            } else {
                std::cerr << "Error: --state-dir requires a directory path\n";
                return std::nullopt;
            }
        } else if (arg == "--runtime-dir") {
            if (i + 1 < argc) {
                opts.runtimeDir = argv[++i];
            } else {
                std::cerr << "Error: --runtime-dir requires a directory path\n";
                return std::nullopt;
            }
        } else {
            std::cerr << "Error: Unrecognized option '" << arg << "'\n";
            return std::nullopt;
        }
    }
    return opts;
}

// Sony headsets also advertise over Bluetooth LE as "LE_<model>", and BlueZ
// lets that overwrite the classic name, so the same headset shows up as
// "LE_WH-1000XM3" after a few reconnects. Show the product name.
std::string displayName(const std::string& bluezName) {
    std::string name = bluezName;
    if (name.rfind("LE_", 0) == 0) name.erase(0, 3);
    return name.empty() ? DEFAULT_DEVICE_NAME : name;
}

std::string resolveStateDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        return overrideDir;
    }
    const char* xdgState = std::getenv("XDG_STATE_HOME");
    if (xdgState && xdgState[0] != '\0') {
        return std::string(xdgState);
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.local/state";
    }
    return "/tmp";
}

std::string resolveRuntimeDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        return overrideDir;
    }
    const char* xdgRun = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRun && xdgRun[0] != '\0') {
        return std::string(xdgRun);
    }
    return "/tmp/run-" + std::to_string(::getuid());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Main Daemon Entry Point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Restrict default permissions on all newly created files and directories
    ::umask(0077);

    auto optsOpt = parseCommandLine(argc, argv);
    if (!optsOpt) {
        return 1;
    }
    const auto& opts = *optsOpt;

    if (opts.showHelp) {
        printHelp(argv[0]);
        return 0;
    }
    if (opts.showVersion) {
        printVersion();
        return 0;
    }

    // 1. Resolve paths
    std::string stateDir = resolveStateDir(opts.stateDir);
    std::string runtimeDir = resolveRuntimeDir(opts.runtimeDir);

    // 2. Setup Linux signalfd (ignoring SIGPIPE, blocking SIGINT/TERM/USR1/USR2)
    ::signal(SIGPIPE, SIG_IGN);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);

    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        std::cerr << "Fatal: Failed to mask signals: " << std::strerror(errno) << "\n";
        return 1;
    }

    int sigFd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigFd < 0) {
        std::cerr << "Fatal: Failed to create signalfd: " << std::strerror(errno) << "\n";
        return 1;
    }

    // 3. Initialize StateEngine
    StateEngine stateEngine(stateDir);
    if (!stateEngine.initialize(opts.mockMode ? true : false)) {
        std::cerr << "Fatal: Failed to initialize StateEngine at " << stateDir << "\n";
        ::close(sigFd);
        return 1;
    }

    // In mock mode, ensure initial standard state
    if (opts.mockMode) {
        stateEngine.setConnected(true, DEFAULT_DEVICE_NAME);
        stateEngine.setBattery(85, false);
        stateEngine.updateNoiseMode("anc", 0, false);
        stateEngine.updateEqPreset("off");
        stateEngine.updateDsee(true);
        stateEngine.setSurround("off");
        stateEngine.setSoundPosition("off");
        stateEngine.setAutoPowerOff("180min");
        stateEngine.setConnectionMode("stable");
        stateEngine.modifyState([](HeadphoneState& s) {
            s.volume = 17;
            s.nc_button = "ambient";
            s.touch_panel = true;
            s.voice_guidance = true;
            s.voice_guidance_language = "English";
            s.optimizer_pressure = "1.0";
            s.firmware_version = "4.5.2";
        });
        stateEngine.save();
    }

    // 4. Initialize BluetoothManager
    BluetoothConfig btConfig;
    btConfig.preferredMac = opts.preferredMac;
    btConfig.autoReconnect = true;
    btConfig.mockMode = opts.mockMode;

    int mockPeerFd = -1;
    std::unique_ptr<BluetoothManager> btManager;
    if (opts.mockMode) {
        btManager = BluetoothManager::createMock(btConfig, &mockPeerFd);
    } else {
        btManager = BluetoothManager::createLinux(btConfig);
    }

    if (!btManager) {
        std::cerr << "Fatal: Failed to create BluetoothManager\n";
        stateEngine.cleanup();
        ::close(sigFd);
        return 1;
    }

    // Protocol stream framer
    StreamFramer streamFramer;

    // SONY_XM3_DEBUG=1 logs every raw byte received, before framing.
    const char* debugEnv = std::getenv("SONY_XM3_DEBUG");
    const bool verboseMdr = debugEnv && debugEnv[0] == '1';

    CommandQueue commands([&](const std::vector<uint8_t>& frame) {
        btManager->sendPacket(frame);
    });
    commands.setLogging(true);

    // Set when the user asks to reassign the NC/AMBIENT button, so that the
    // headset's "this will disconnect — proceed?" alert for exactly that change
    // is answered yes. Any other alert is declined: nothing should disconnect
    // the headset that the user did not ask for.
    bool keyAssignRequested = false;

    // Remembers the last true ambient step so that switching ANC -> Ambient
    // returns to where the user left the slider rather than to a default.
    int lastAmbientStep = -1;

    auto connected = [&]() {
        return btManager && btManager->getState() == ConnectionState::CONNECTED;
    };

    auto send = [&](const std::vector<uint8_t>& packet, const char* what, bool coalesce = false) {
        if (connected()) {
            commands.enqueue(packet, what, coalesce);
        } else {
            fprintf(stderr, "[DAEMON] Not connected; dropped %s\n", what);
            fflush(stderr);
        }
    };

    BluetoothCallbacks callbacks;
    callbacks.onConnected = [&]() {
        commands.reset();
        const auto& dev = btManager->getCurrentDevice();
        std::string name = displayName(dev.name);
        stateEngine.setConnected(true, name);
        if (dev.batteryLevel >= 0) {
            stateEngine.setBattery(dev.batteryLevel, false);
        }
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Headset connected (%s, battery: %d%%)\n",
                name.c_str(), dev.batteryLevel);
        fflush(stderr);

        if (!opts.mockMode) {
            // Handshake first: the headset ignores parameter queries until it
            // has seen these. Then the state queries, one ACK at a time.
            send(serializeQueryProtocolInfo(), "handshake protocol info");
            send(serializeQueryCapabilityInfo(), "handshake capability info");
            send(serializeQuerySupportFunction(), "handshake support function");
            send(serializeQueryModelName(), "query model name");
            send(serializeQueryFirmwareVersion(), "query firmware version");
            send(serializeQueryNcAsmCapability(), "query nc/asm capability");
            send(serializeQueryBattery(), "query battery");
            send(serializeQueryNoiseMode(), "query noise mode");
            send(serializeQueryEq(), "query eq");
            send(serializeQueryCodec(), "query codec");
            send(serializeQueryDsee(), "query dsee hx");
            send(serializeQuerySurround(), "query surround");
            send(serializeQuerySoundPosition(), "query sound position");
            send(serializeQueryAutoPowerOff(), "query auto power off");
            send(serializeQueryConnectionMode(), "query connection mode");
            send(serializeQueryOptimizerStatus(), "query optimizer status");
            send(serializeQueryOptimizerParam(), "query optimizer result");
            send(serializeQueryPlaybackCapability(), "query playback capability");
            send(serializeQueryVolume(), "query volume");
            send(serializeQueryNcButton(), "query nc button");
            send(serializeQueryTouchPanel(), "query touch panel");
            send(serializeQueryVoiceGuidance(), "query voice guidance");
            send(serializeQueryVoiceGuidanceLanguage(), "query voice guidance language");
        }
    };

    callbacks.onDisconnected = [&](const std::string& reason) {
        commands.reset();
        streamFramer.reset();
        stateEngine.setConnected(false);
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Headset disconnected: %s\n", reason.c_str());
        fflush(stderr);
    };

    callbacks.onDataReceived = [&](const uint8_t* data, size_t length) {
        if (verboseMdr) {
            fprintf(stderr, "[MDR] RX raw %zu bytes:", length);
            for (size_t i = 0; i < length; ++i) fprintf(stderr, " %02x", data[i]);
            fprintf(stderr, "\n");
        }
        streamFramer.append(std::span<const uint8_t>(data, length));
        while (auto frameOpt = streamFramer.nextFrame()) {
            auto unpackedOpt = unpackFrame(*frameOpt);
            if (!unpackedOpt) {
                fprintf(stderr, "[MDR] RX undecodable frame (%zu bytes):", frameOpt->size());
                for (uint8_t b : *frameOpt) fprintf(stderr, " %02x", b);
                fprintf(stderr, "\n");
                fflush(stderr);
                continue;
            }

            const auto& unpacked = *unpackedOpt;
            if (unpacked.type == PacketType::ACK) {
                // Logged before onFrame(): the ACK releases the next command,
                // and its TX line should read as a consequence, not a cause.
                fprintf(stderr, "[MDR] RX ACK seq=%u\n", static_cast<unsigned>(unpacked.seq));
                fflush(stderr);
            }
            commands.onFrame(unpacked.type, unpacked.seq);
            if (unpacked.type == PacketType::ACK) {
                continue;
            }
            if (unpacked.type == PacketType::DATA_MDR || unpacked.type == PacketType::DATA_MDR_NO2) {
                // Reply with ACK packet
                auto ack = serializeACK(unpacked.seq);
                btManager->sendPacket(ack);

                fprintf(stderr, "[DAEMON] RX%s payload (cmd=0x%02x, size=%zu): ",
                        unpacked.type == PacketType::DATA_MDR_NO2 ? " T2" : "",
                        unpacked.payload.empty() ? 0 : unpacked.payload[0], unpacked.payload.size());
                for (uint8_t b : unpacked.payload) {
                    fprintf(stderr, "%02x ", b);
                }
                fprintf(stderr, "\n");
                fflush(stderr);

                // Update state from payload
                bool changed = unpacked.type == PacketType::DATA_MDR_NO2
                    ? protocol::parseInboundPayloadT2(unpacked.payload, stateEngine.getStateUnsafe())
                    : protocol::parseInboundPayload(unpacked.payload, stateEngine.getStateUnsafe());
                // A "proceed?" alert: [0x99, FIXED_MESSAGE, message, POSITIVE_NEGATIVE]
                if (unpacked.type == PacketType::DATA_MDR && unpacked.payload.size() >= 4 &&
                    unpacked.payload[0] == static_cast<uint8_t>(Command::ALERT_NTFY_PARAM) &&
                    unpacked.payload[1] == 0x01 && unpacked.payload[3] == 0x01) {
                    const uint8_t message = unpacked.payload[2];
                    const bool proceed = (message == kAlertKeyAssignChange && keyAssignRequested);
                    keyAssignRequested = false;
                    fprintf(stderr, "[DAEMON] Headset asked to confirm alert 0x%02x; answering %s\n",
                            message, proceed ? "yes" : "no");
                    fflush(stderr);
                    send(serializeAlertReply(message, proceed), "alert reply");
                }

                if (changed) {
                    stateEngine.save();
                    const auto snapshot = stateEngine.getState();
                    if (snapshot.noise_mode == "ambient") {
                        lastAmbientStep = snapshot.ambient_sound_level;
                    }
                    fprintf(stderr, "[DAEMON] Updated state: mode=%s, level=%d, voice=%d\n",
                            snapshot.noise_mode.c_str(),
                            snapshot.ambient_sound_level,
                            snapshot.voice_passthrough ? 1 : 0);
                    fflush(stderr);
                }
            }
        }
    };

    btManager->setCallbacks(std::move(callbacks));
    btManager->start();

    // 5. Initialize UNIX Domain Socket IPC Server
    std::string socketPath = runtimeDir + "/sony-xm3.sock";
    IpcServer ipcServer(socketPath);

    IpcCallbacks ipcCb;
    ipcCb.getStatusJson = [&]() {
        return stateEngine.getStatusJson();
    };
    ipcCb.getAmbientMaxLevel = [&]() {
        return stateEngine.getState().ambient_max_level;
    };
    ipcCb.setNoiseMode = [&](NoiseMode mode, uint8_t ambientLevel, std::string& /*err*/) {
        const auto snapshot = stateEngine.getState();
        const bool voiceFocus = snapshot.voice_passthrough;

        uint8_t step;
        if (mode == NoiseMode::AMBIENT) {
            const int preferred = (ambientLevel >= kMinAmbientStep)
                ? static_cast<int>(ambientLevel)
                : lastAmbientStep;
            step = noiseModeToStep(mode, preferred, snapshot.ambient_max_level);
            lastAmbientStep = step;
        } else {
            step = noiseModeToStep(mode, 0, snapshot.ambient_max_level);
        }

        stateEngine.updateNoiseMode(noiseModeToString(mode), step, voiceFocus);
        stateEngine.save();

        fprintf(stderr, "[DAEMON] Noise mode: %s (step %u)\n",
                noiseModeToString(mode).c_str(), (unsigned)step);
        fflush(stderr);

        if (mode == NoiseMode::OFF) {
            send(serializeNcAsm(false, step, false), "noise off", true);
        } else {
            send(serializeNcAsm(true, step, voiceFocus), "noise mode", true);
        }
        return true;
    };
    ipcCb.setAmbientLevel = [&](uint8_t level, std::string& /*err*/) {
        const bool voiceFocus = stateEngine.getState().voice_passthrough;
        if (level >= kMinAmbientStep) {
            lastAmbientStep = level;
        }
        stateEngine.updateAmbientLevel(level);
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Ambient step: %u\n", (unsigned)level);
        fflush(stderr);
        send(serializeAmbientLevel(level, voiceFocus), "ambient level", true);
        return true;
    };
    ipcCb.setVoiceFocus = [&](bool enabled, std::string& /*err*/) {
        stateEngine.setVoicePassthrough(enabled);
        stateEngine.save();
        const auto snapshot = stateEngine.getState();
        // Focus on Voice rides along with the NC/ASM command, so re-send the
        // current step rather than inventing a separate message for it.
        const uint8_t step = static_cast<uint8_t>(snapshot.ambient_sound_level);
        const bool noiseOn = snapshot.noise_mode != "off";
        send(serializeNcAsm(noiseOn, step, enabled), "focus on voice", true);
        return true;
    };
    // On "Priority on sound quality" (LDAC) the XM3 cannot run its EQ or VPT
    // processing. A command sent anyway is answered with a "this will change
    // the connection mode — proceed?" alert rather than applied, so refuse it
    // up front and say why.
    auto dspBlockedByLdac = [&](const char* feature, std::string& err) {
        if (stateEngine.getState().connection_mode != "quality") return false;
        err = std::string(feature) +
              " is unavailable on Priority on sound quality (LDAC); "
              "use `sony-xm3-ctl connection stable` to trade LDAC for it";
        return true;
    };

    ipcCb.setEqPreset = [&](EqPreset preset, std::string& err) {
        if (dspBlockedByLdac("EQ", err)) return false;
        stateEngine.updateEqPreset(eqPresetToString(preset));
        stateEngine.save();
        send(serializeEqPreset(preset), "eq preset");
        return true;
    };
    ipcCb.setCustomEq = [&](EqPreset slot, const std::array<int, 5>& bands, int clearBass, std::string& err) {
        if (dspBlockedByLdac("EQ", err)) return false;
        stateEngine.updateCustomEq(bands, clearBass, eqPresetToString(slot));
        stateEngine.save();
        send(serializeCustomEq(bands, clearBass, 0, slot), "custom eq", true);
        return true;
    };
    ipcCb.setDsee = [&](bool enabled, std::string& /*err*/) {
        stateEngine.updateDsee(enabled);
        stateEngine.save();
        send(serializeDsee(enabled), "dsee hx");
        return true;
    };
    ipcCb.setSurround = [&](SurroundPreset preset, std::string& err) {
        if (dspBlockedByLdac("Surround", err)) return false;
        stateEngine.setSurround(surroundToString(preset));
        stateEngine.save();
        send(serializeSurround(preset), "surround");
        return true;
    };
    ipcCb.setSoundPosition = [&](SoundPosition position, std::string& err) {
        if (dspBlockedByLdac("Sound position", err)) return false;
        stateEngine.setSoundPosition(soundPositionToString(position));
        stateEngine.save();
        send(serializeSoundPosition(position), "sound position");
        return true;
    };
    ipcCb.setAutoPowerOff = [&](AutoPowerOff timer, std::string& /*err*/) {
        stateEngine.setAutoPowerOff(autoPowerOffToString(timer));
        stateEngine.save();
        send(serializeAutoPowerOff(timer), "auto power off");
        return true;
    };
    ipcCb.setConnectionMode = [&](ConnectionMode mode, std::string& /*err*/) {
        stateEngine.setConnectionMode(connectionModeToString(mode));
        stateEngine.save();
        send(serializeConnectionMode(mode), "connection mode");
        send(serializeQueryEq(), "re-query eq after mode change");
        send(serializeQuerySurround(), "re-query surround after mode change");
        send(serializeQuerySoundPosition(), "re-query sound position after mode change");
        return true;
    };
    ipcCb.setOptimizer = [&](bool start, std::string& /*err*/) {
        // Progress and the result arrive as notifications; the headset has to
        // be worn, because it measures fit by playing test tones.
        stateEngine.modifyState([&](HeadphoneState& s) {
            s.optimizer_state = start ? "measuring-fit" : "idle";
        });
        send(serializeOptimizer(start), start ? "optimizer start" : "optimizer cancel");
        return true;
    };
    ipcCb.getVolumeMax = [&]() { return stateEngine.getState().volume_max; };
    ipcCb.setVolume = [&](uint8_t volume, std::string& /*err*/) {
        stateEngine.modifyState([&](HeadphoneState& s) { s.volume = volume; });
        send(serializeVolume(volume), "volume", true);
        return true;
    };
    ipcCb.setPlayback = [&](PlaybackControl control, std::string& /*err*/) {
        send(serializePlayback(control), "playback");
        return true;
    };
    ipcCb.setNcButton = [&](NcButton button, std::string& /*err*/) {
        keyAssignRequested = true;
        stateEngine.modifyState([&](HeadphoneState& s) { s.nc_button = ncButtonToString(button); });
        send(serializeNcButton(button), "nc button");
        return true;
    };
    ipcCb.setTouchPanel = [&](bool enabled, std::string& /*err*/) {
        stateEngine.modifyState([&](HeadphoneState& s) { s.touch_panel = enabled; });
        send(serializeTouchPanel(enabled), "touch panel");
        return true;
    };
    ipcCb.setVoiceGuidance = [&](bool enabled, std::string& /*err*/) {
        stateEngine.modifyState([&](HeadphoneState& s) { s.voice_guidance = enabled; });
        send(serializeVoiceGuidance(enabled), "voice guidance");
        return true;
    };
    ipcCb.sendPacket = [&](const std::vector<uint8_t>& packet) {
        send(packet, "raw packet");
        return true;
    };
    ipcCb.onTestSetBattery = [&](int level, bool charging) {
        stateEngine.setBattery(level, charging);
        stateEngine.save();
    };
    ipcCb.onTestDisconnect = [&]() {
        stateEngine.setConnected(false);
        stateEngine.save();
    };
    ipcCb.onTestReconnect = [&]() {
        stateEngine.setConnected(true);
        stateEngine.save();
    };

    ipcServer.setCallbacks(std::move(ipcCb));

    if (!ipcServer.start()) {
        std::cerr << "Fatal: Failed to start IPC server at " << socketPath << "\n";
        btManager->stop();
        stateEngine.cleanup();
        ::close(sigFd);
        return 1;
    }

    // 6. Signal daemon readiness
    if (opts.mockMode) {
        std::cout << "[MOCK_DAEMON] Ready PID=" << ::getpid() << std::endl;
    } else {
        std::cout << "[DAEMON] Ready PID=" << ::getpid() << std::endl;
    }

    // 7. Unified Event Loop
    bool running = true;

    while (running) {
        std::vector<struct pollfd> pfds;

        // Entry 0: Signal descriptor
        pfds.push_back({sigFd, POLLIN, 0});

        // Entry 1 (Optional): Bluetooth transport descriptor
        int btFd = btManager->getPollFd();
        short btEvents = btManager->getPollEvents();
        int btIndex = -1;
        if (btFd >= 0 && btEvents != 0) {
            btIndex = static_cast<int>(pfds.size());
            pfds.push_back({btFd, btEvents, 0});
        }

        // Entries 2+: IPC listen socket and connected client sockets
        size_t ipcStartIndex = pfds.size();
        ipcServer.appendPollFds(pfds);

        // Poll with 100ms timeout for periodic BluetoothManager tick
        int pollRc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
        if (pollRc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Event loop poll error: " << std::strerror(errno) << "\n";
            break;
        }

        // Check Signal Descriptor
        if (pfds[0].revents & POLLIN) {
            struct signalfd_siginfo fdsi{};
            ssize_t s = ::read(sigFd, &fdsi, sizeof(fdsi));
            if (s == sizeof(fdsi)) {
                if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM) {
                    running = false;
                    break;
                } else if (fdsi.ssi_signo == SIGUSR1) {
                    // Simulated disconnect
                    stateEngine.setConnected(false);
                    stateEngine.save();
                    if (!opts.mockMode) {
                        btManager->disconnect();
                    }
                } else if (fdsi.ssi_signo == SIGUSR2) {
                    // Simulated reconnect
                    stateEngine.setConnected(true);
                    stateEngine.save();
                    if (!opts.mockMode) {
                        btManager->start();
                    }
                }
            }
        }

        // Check Bluetooth Transport Descriptor
        if (btIndex >= 0 && (pfds[btIndex].revents != 0)) {
            btManager->handleSocketEvent(pfds[btIndex].revents);
        }

        // Check IPC Descriptors
        for (size_t i = ipcStartIndex; i < pfds.size(); ++i) {
            if (pfds[i].revents != 0) {
                ipcServer.handleSocketEvent(pfds[i].fd, pfds[i].revents);
            }
        }

        // Subsystem periodic tick (timeouts and reconnect backoff)
        btManager->tick();
        commands.tick();
    }

    // 8. Graceful Shutdown & Resource Cleanup
    if (opts.mockMode) {
        std::cout << "[MOCK_DAEMON] Shutdown cleanly\n";
    } else {
        std::cout << "[DAEMON] Shutdown cleanly\n";
    }
    std::cout.flush();

    btManager->stop();
    ipcServer.stop();
    stateEngine.cleanup();

    if (mockPeerFd >= 0) {
        ::close(mockPeerFd);
        mockPeerFd = -1;
    }
    if (sigFd >= 0) {
        ::close(sigFd);
        sigFd = -1;
    }

    return 0;
}
