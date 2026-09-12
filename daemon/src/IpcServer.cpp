#include "IpcServer.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace omarchy::sony {

namespace {

// Helper: Trim trailing whitespace and newlines
std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.size() && (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n')) {
        start++;
    }
    if (start == str.size()) return "";
    size_t end = str.size();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n')) {
        end--;
    }
    return str.substr(start, end - start);
}

// Helper: Convert string to lowercase
std::string toLower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Helper: Split line into whitespace-separated tokens
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// Helper: Safely parse integer with full string consumption check
bool parseInt(const std::string& str, int& outVal) {
    if (str.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(str.c_str(), &end, 10);
    if (errno != 0 || end == str.c_str() || *end != '\0') {
        return false;
    }
    outVal = static_cast<int>(val);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Path Resolution
// ---------------------------------------------------------------------------

// Must stay in step with get_default_socket_path() in cli/src/main.cpp: the two
// halves only find each other if they agree on this name.
static constexpr const char* kSocketName = "sony-xm3.sock";

std::string IpcServer::resolveSocketPath(const std::string& overridePath) {
    if (!overridePath.empty()) {
        std::filesystem::path p(overridePath);
        if (p.extension() == ".sock") {
            return overridePath;
        }
        return (p / kSocketName).string();
    }

    const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRuntime != nullptr && *xdgRuntime != '\0') {
        return std::string(xdgRuntime) + "/" + kSocketName;
    }

    // Fallback: /tmp/sony-xm3-$UID.sock
    return "/tmp/sony-xm3-" + std::to_string(::getuid()) + ".sock";
}

// ---------------------------------------------------------------------------
// Construction & Lifecycle
// ---------------------------------------------------------------------------

IpcServer::IpcServer(std::string socketPath)
    : socketPathConfig_(std::move(socketPath)) {
    actualSocketPath_ = resolveSocketPath(socketPathConfig_);
}

IpcServer::~IpcServer() {
    stop();
}

IpcServer::IpcServer(IpcServer&& other) noexcept
    : socketPathConfig_(std::move(other.socketPathConfig_)),
      actualSocketPath_(std::move(other.actualSocketPath_)),
      listenFd_(other.listenFd_),
      running_(other.running_),
      seq_(other.seq_),
      maxLineLength_(other.maxLineLength_),
      customHandler_(std::move(other.customHandler_)),
      callbacks_(std::move(other.callbacks_)),
      clients_(std::move(other.clients_)) {
    other.listenFd_ = -1;
    other.running_ = false;
}

IpcServer& IpcServer::operator=(IpcServer&& other) noexcept {
    if (this != &other) {
        stop();
        socketPathConfig_ = std::move(other.socketPathConfig_);
        actualSocketPath_ = std::move(other.actualSocketPath_);
        listenFd_ = other.listenFd_;
        running_ = other.running_;
        seq_ = other.seq_;
        maxLineLength_ = other.maxLineLength_;
        customHandler_ = std::move(other.customHandler_);
        callbacks_ = std::move(other.callbacks_);
        clients_ = std::move(other.clients_);

        other.listenFd_ = -1;
        other.running_ = false;
    }
    return *this;
}

bool IpcServer::start() {
    if (running_) {
        return true;
    }

    if (actualSocketPath_.empty()) {
        actualSocketPath_ = resolveSocketPath(socketPathConfig_);
    }

    // 1. Ensure parent directory exists with mode 0700
    std::filesystem::path sockPath(actualSocketPath_);
    if (sockPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(sockPath.parent_path(), ec);
        if (!ec) {
            ::chmod(sockPath.parent_path().c_str(), 0700);
        }
    }

    // 2. Unlink any stale existing socket file
    ::unlink(actualSocketPath_.c_str());

    // 3. Create non-blocking stream socket
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        return false;
    }

    // 4. Bind socket
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (actualSocketPath_.length() >= sizeof(addr.sun_path)) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, actualSocketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 5. Enforce 0700 socket permissions
    if (::chmod(actualSocketPath_.c_str(), 0700) < 0) {
        ::unlink(actualSocketPath_.c_str());
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 6. Listen
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        ::unlink(actualSocketPath_.c_str());
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    return true;
}

void IpcServer::stop() {
    if (!running_ && listenFd_ < 0 && clients_.empty()) {
        return;
    }

    for (auto& [fd, session] : clients_) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    clients_.clear();

    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }

    if (!actualSocketPath_.empty()) {
        ::unlink(actualSocketPath_.c_str());
    }

    running_ = false;
}

// ---------------------------------------------------------------------------
// Client Management & I/O
// ---------------------------------------------------------------------------

void IpcServer::acceptClients() {
    while (running_ && listenFd_ >= 0) {
        struct sockaddr_un clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = ::accept4(listenFd_, reinterpret_cast<struct sockaddr*>(&clientAddr),
                                &addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        ClientSession session;
        session.fd = clientFd;
        session.connectedAt = std::chrono::steady_clock::now();
        clients_[clientFd] = std::move(session);
    }
}

void IpcServer::handleClientRead(int clientFd) {
    auto it = clients_.find(clientFd);
    if (it == clients_.end()) {
        return;
    }
    ClientSession& session = it->second;

    char buf[2048];
    while (true) {
        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n > 0) {
            session.inBuffer.append(buf, static_cast<size_t>(n));

            if (session.inBuffer.size() > maxLineLength_) {
                queueResponse(session, "ERR line too long\n");
                char drainBuf[8192];
                while (::recv(clientFd, drainBuf, sizeof(drainBuf), MSG_DONTWAIT) > 0) {}
                closeClient(clientFd);
                return;
            }

            size_t pos;
            while ((pos = session.inBuffer.find('\n')) != std::string::npos) {
                std::string line = session.inBuffer.substr(0, pos);
                session.inBuffer.erase(0, pos + 1);

                // `subscribe` needs the session, so it is handled here rather
                // than in the session-independent command parser.
                std::string response;
                if (trim(line) == "subscribe") {
                    session.subscribed = true;
                    response = callbacks_.getStatusJson ? callbacks_.getStatusJson() : std::string("{}");
                    if (response.empty() || response.back() != '\n') {
                        response += "\n";
                    }
                } else if (trim(line) == "unsubscribe") {
                    session.subscribed = false;
                    response = "OK\n";
                } else {
                    response = handleCommandLine(line);
                }
                if (!queueResponse(session, response)) {
                    closeClient(clientFd);
                    return;
                }
            }
        } else if (n == 0) {
            closeClient(clientFd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            closeClient(clientFd);
            return;
        }
    }
}

bool IpcServer::flushClientOutBuffer(ClientSession& session) {
    while (!session.outBuffer.empty()) {
        ssize_t sent = ::send(session.fd, session.outBuffer.data(), session.outBuffer.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            session.outBuffer.erase(0, static_cast<size_t>(sent));
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        } else {
            return true;
        }
    }
    return true;
}

void IpcServer::handleClientWrite(int clientFd) {
    auto it = clients_.find(clientFd);
    if (it != clients_.end()) {
        if (!flushClientOutBuffer(it->second)) {
            closeClient(clientFd);
        }
    }
}

bool IpcServer::queueResponse(ClientSession& session, const std::string& response) {
    session.outBuffer.append(response);
    return flushClientOutBuffer(session);
}

void IpcServer::broadcastStatus(const std::string& statusJson) {
    std::string line = statusJson;
    if (line.empty() || line.back() != '\n') {
        line += "\n";
    }

    std::vector<int> stalled;
    for (auto& [fd, session] : clients_) {
        if (!session.subscribed) {
            continue;
        }
        // A reader that never drains would otherwise queue without bound.
        if (session.outBuffer.size() + line.size() > maxOutBuffer_) {
            stalled.push_back(fd);
            continue;
        }
        if (!queueResponse(session, line)) {
            stalled.push_back(fd);
        }
    }
    for (int fd : stalled) {
        closeClient(fd);
    }
}

size_t IpcServer::getSubscriberCount() const noexcept {
    size_t count = 0;
    for (const auto& [fd, session] : clients_) {
        if (session.subscribed) {
            ++count;
        }
    }
    return count;
}

void IpcServer::closeClient(int clientFd) {
    auto it = clients_.find(clientFd);
    if (it != clients_.end()) {
        ::close(it->first);
        clients_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Event Loop Integration
// ---------------------------------------------------------------------------

void IpcServer::appendPollFds(std::vector<struct pollfd>& pfds) const {
    if (listenFd_ >= 0) {
        pfds.push_back({listenFd_, POLLIN, 0});
    }
    for (const auto& [fd, session] : clients_) {
        short events = POLLIN;
        if (!session.outBuffer.empty()) {
            events |= POLLOUT;
        }
        pfds.push_back({fd, events, 0});
    }
}

void IpcServer::handleSocketEvent(int fd, short revents) {
    if (fd == listenFd_) {
        if (revents & (POLLIN | POLLERR)) {
            acceptClients();
        }
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        closeClient(fd);
        return;
    }

    if (revents & POLLOUT) {
        handleClientWrite(fd);
    }

    if ((revents & POLLIN) && clients_.find(fd) != clients_.end()) {
        handleClientRead(fd);
    }
}

void IpcServer::handlePollEvents(std::span<const struct pollfd> pfds) {
    std::vector<int> clientsToClose;

    for (const auto& pfd : pfds) {
        if (pfd.fd == listenFd_) {
            if (pfd.revents & (POLLIN | POLLERR)) {
                acceptClients();
            }
            continue;
        }

        auto it = clients_.find(pfd.fd);
        if (it == clients_.end()) {
            continue;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            clientsToClose.push_back(pfd.fd);
            continue;
        }

        if (pfd.revents & POLLOUT) {
            handleClientWrite(pfd.fd);
        }

        if ((pfd.revents & POLLIN) && clients_.find(pfd.fd) != clients_.end()) {
            handleClientRead(pfd.fd);
        }
    }

    for (int fd : clientsToClose) {
        closeClient(fd);
    }
}

int IpcServer::pollOnce(int timeoutMs) {
    std::vector<pollfd> pfds;
    appendPollFds(pfds);
    if (pfds.empty()) {
        return 0;
    }

    int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
    if (ret > 0) {
        handlePollEvents(pfds);
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Command Execution
// ---------------------------------------------------------------------------

std::string IpcServer::handleCommandLine(const std::string& line) {
    if (customHandler_) {
        return customHandler_(line);
    }
    return handleBuiltinCommand(line);
}

std::string IpcServer::handleBuiltinCommand(const std::string& line) {
    std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return "ERR empty command\n";
    }

    auto tokens = tokenize(trimmed);
    if (tokens.empty()) {
        return "ERR empty command\n";
    }

    std::string verb = toLower(tokens[0]);

    // 1. status
    if (verb == "status") {
        if (callbacks_.getStatusJson) {
            std::string json = callbacks_.getStatusJson();
            if (json.empty() || json.back() != '\n') {
                json += "\n";
            }
            return json;
        }
        return "{\"schema_version\":1,\"connected\":false}\n";
    }

    // 2. noise <anc|wind|ambient|off>
    if (verb == "noise") {
        if (tokens.size() < 2) {
            return "ERR missing mode (expected anc|wind|ambient|off)\n";
        }
        std::string modeStr = toLower(tokens[1]);
        if (modeStr != "anc" && modeStr != "ambient" && modeStr != "wind" && modeStr != "off") {
            return "ERR invalid mode '" + modeStr + "'\n";
        }

        protocol::NoiseMode mode = protocol::stringToNoiseMode(modeStr);
        // 0 means "no explicit level"; the daemon substitutes the remembered one.
        uint8_t ambientLevel = 0;
        if (modeStr == "ambient" && tokens.size() >= 3) {
            int lvl = 0;
            if (!parseInt(tokens[2], lvl)) {
                return "ERR invalid ambient level '" + tokens[2] + "'\n";
            }
            const int maxLevel = callbacks_.getAmbientMaxLevel
                ? callbacks_.getAmbientMaxLevel() : protocol::kMaxAmbientStep;
            if (lvl < protocol::kMinAmbientStep || lvl > maxLevel) {
                return "ERR ambient level out of range [" +
                       std::to_string(static_cast<int>(protocol::kMinAmbientStep)) + "-" +
                       std::to_string(maxLevel) + "]\n";
            }
            ambientLevel = static_cast<uint8_t>(lvl);
        }

        if (callbacks_.setNoiseMode) {
            std::string err;
            if (!callbacks_.setNoiseMode(mode, ambientLevel, err)) {
                return "ERR " + (err.empty() ? "failed to set noise mode" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 3. ambient-level <0..max>
    //    On the XM3 the level and the mode are the same axis: step 0 is noise
    //    cancelling, step 1 is wind noise reduction, and 2..max are ambient.
    if (verb == "ambient-level") {
        const int maxLevel = callbacks_.getAmbientMaxLevel
            ? callbacks_.getAmbientMaxLevel() : protocol::kMaxAmbientStep;
        if (tokens.size() < 2) {
            return "ERR missing level (0-" + std::to_string(maxLevel) + ")\n";
        }
        int level = 0;
        if (!parseInt(tokens[1], level)) {
            return "ERR invalid level '" + tokens[1] + "'\n";
        }
        if (level < 0 || level > maxLevel) {
            return "ERR ambient level out of range [0-" + std::to_string(maxLevel) + "]\n";
        }

        if (callbacks_.setAmbientLevel) {
            std::string err;
            if (!callbacks_.setAmbientLevel(static_cast<uint8_t>(level), err)) {
                return "ERR " + (err.empty() ? "failed to set ambient level" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 4. eq <preset> OR eq custom <b1> <b2> <b3> <b4> <b5> <cb>
    if (verb == "eq") {
        if (tokens.size() < 2) {
            return "ERR missing eq preset\n";
        }
        std::string presetStr = toLower(tokens[1]);

        const bool isSlot = presetStr == "custom" || presetStr == "manual" ||
                            presetStr == "user1" || presetStr == "user2";
        // A slot name alone selects it; followed by six numbers it also sets
        // that slot's bands.
        if (isSlot && tokens.size() > 2) {
            if (tokens.size() < 8) {
                return "ERR custom eq requires 5 bands and clear bass (6 integers [-10, 10])\n";
            }
            std::array<int, 5> bands{};
            for (size_t i = 0; i < 5; ++i) {
                int b = 0;
                if (!parseInt(tokens[2 + i], b)) {
                    return "ERR invalid custom eq arguments\n";
                }
                if (b < -10 || b > 10) {
                    return "ERR custom eq band out of range [-10, 10]\n";
                }
                bands[i] = b;
            }

            int clearBass = 0;
            if (!parseInt(tokens[7], clearBass)) {
                return "ERR invalid custom eq arguments\n";
            }
            if (clearBass < -10 || clearBass > 10) {
                return "ERR clear bass out of range [-10, 10]\n";
            }

            const auto slot = presetStr == "user1" ? protocol::EqPreset::USER1
                            : presetStr == "user2" ? protocol::EqPreset::USER2
                                                   : protocol::EqPreset::CUSTOM;
            if (callbacks_.setCustomEq) {
                std::string err;
                if (!callbacks_.setCustomEq(slot, bands, clearBass, err)) {
                    return "ERR " + (err.empty() ? "failed to set custom eq" : err) + "\n";
                }
            }
            return "OK\n";
        } else {
            static const std::vector<std::string> validPresets = {
                "off", "bright", "excited", "mellow", "relaxed",
                "vocal", "treble", "bass", "speech", "custom", "manual", "user1", "user2"
            };
            bool found = false;
            for (const auto& p : validPresets) {
                if (presetStr == p) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return "ERR unknown eq preset '" + presetStr + "'\n";
            }

            protocol::EqPreset preset = protocol::stringToEqPreset(presetStr);
            if (callbacks_.setEqPreset) {
                std::string err;
                if (!callbacks_.setEqPreset(preset, err)) {
                    return "ERR " + (err.empty() ? "failed to set eq preset" : err) + "\n";
                }
            }
            return "OK\n";
        }
    }

    // 5. Boolean feature toggles
    if (verb == "voice-focus" || verb == "dsee") {
        if (tokens.size() < 2) {
            return "ERR expected on|off\n";
        }
        std::string val = toLower(tokens[1]);
        if (val != "on" && val != "off") {
            return "ERR expected on|off\n";
        }
        const bool enabled = (val == "on");
        std::string err;

        if (verb == "voice-focus") {
            if (callbacks_.setVoiceFocus && !callbacks_.setVoiceFocus(enabled, err)) {
                return "ERR " + (err.empty() ? "failed to set focus on voice" : err) + "\n";
            }
        } else {
            if (callbacks_.setDsee && !callbacks_.setDsee(enabled, err)) {
                return "ERR " + (err.empty() ? "failed to set dsee" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 6. surround <off|outdoor|arena|concert|club>
    if (verb == "surround") {
        if (tokens.size() < 2) {
            return "ERR expected off|outdoor|arena|concert|club\n";
        }
        auto preset = protocol::stringToSurround(toLower(tokens[1]));
        if (preset == protocol::SurroundPreset::UNKNOWN) {
            return "ERR unknown surround preset '" + tokens[1] + "'\n";
        }
        if (callbacks_.setSurround) {
            std::string err;
            if (!callbacks_.setSurround(preset, err)) {
                return "ERR " + (err.empty() ? "failed to set surround" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 7. sound-position <off|front-left|front-right|front|rear-left|rear-right>
    if (verb == "sound-position") {
        if (tokens.size() < 2) {
            return "ERR expected off|front-left|front-right|front|rear-left|rear-right\n";
        }
        auto position = protocol::stringToSoundPosition(toLower(tokens[1]));
        if (position == protocol::SoundPosition::UNKNOWN) {
            return "ERR unknown sound position '" + tokens[1] + "'\n";
        }
        if (callbacks_.setSoundPosition) {
            std::string err;
            if (!callbacks_.setSoundPosition(position, err)) {
                return "ERR " + (err.empty() ? "failed to set sound position" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 8. auto-power-off <off|5min|30min|60min|180min>
    if (verb == "auto-power-off") {
        if (tokens.size() < 2) {
            return "ERR expected off|5min|30min|60min|180min\n";
        }
        auto timer = protocol::stringToAutoPowerOff(toLower(tokens[1]));
        if (timer == protocol::AutoPowerOff::UNKNOWN) {
            return "ERR unknown auto power off value '" + tokens[1] + "'\n";
        }
        if (callbacks_.setAutoPowerOff) {
            std::string err;
            if (!callbacks_.setAutoPowerOff(timer, err)) {
                return "ERR " + (err.empty() ? "failed to set auto power off" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 9. connection <quality|stable>
    if (verb == "connection") {
        if (tokens.size() < 2) {
            return "ERR expected quality|stable\n";
        }
        auto mode = protocol::stringToConnectionMode(toLower(tokens[1]));
        if (mode == protocol::ConnectionMode::UNKNOWN) {
            return "ERR unknown connection mode '" + tokens[1] + "'\n";
        }
        if (callbacks_.setConnectionMode) {
            std::string err;
            if (!callbacks_.setConnectionMode(mode, err)) {
                return "ERR " + (err.empty() ? "failed to set connection mode" : err) + "\n";
            }
        }
        return "OK\n";
    }

    // 10. optimizer <start|cancel>  — NC Optimizer (wear the headset: it plays test tones)
    if (verb == "optimizer") {
        const std::string val = tokens.size() >= 2 ? toLower(tokens[1]) : "";
        if (val != "start" && val != "cancel") {
            return "ERR expected start|cancel\n";
        }
        std::string err;
        if (callbacks_.setOptimizer && !callbacks_.setOptimizer(val == "start", err)) {
            return "ERR " + (err.empty() ? "failed to control the optimizer" : err) + "\n";
        }
        return "OK\n";
    }

    // 11. volume <0..max>  — the headset's own volume (AVRCP absolute volume)
    if (verb == "volume") {
        const int maxVolume = callbacks_.getVolumeMax ? callbacks_.getVolumeMax() : 30;
        int level = 0;
        if (tokens.size() < 2 || !parseInt(tokens[1], level)) {
            return "ERR expected a volume 0-" + std::to_string(maxVolume) + "\n";
        }
        if (level < 0 || level > maxVolume) {
            return "ERR volume out of range [0-" + std::to_string(maxVolume) + "]\n";
        }
        std::string err;
        if (callbacks_.setVolume && !callbacks_.setVolume(static_cast<uint8_t>(level), err)) {
            return "ERR " + (err.empty() ? "failed to set volume" : err) + "\n";
        }
        return "OK\n";
    }

    // 12. playback <play|pause|next|previous>
    if (verb == "playback") {
        const auto control = protocol::stringToPlayback(tokens.size() >= 2 ? toLower(tokens[1]) : "");
        if (control == protocol::PlaybackControl::UNKNOWN) {
            return "ERR expected play|pause|next|previous\n";
        }
        std::string err;
        if (callbacks_.setPlayback && !callbacks_.setPlayback(control, err)) {
            return "ERR " + (err.empty() ? "failed to send playback control" : err) + "\n";
        }
        return "OK\n";
    }

    // 13. nc-button <ambient|google-assistant|alexa>  — what the NC/AMBIENT button does
    if (verb == "nc-button") {
        const auto button = protocol::stringToNcButton(tokens.size() >= 2 ? toLower(tokens[1]) : "");
        if (button == protocol::NcButton::UNKNOWN) {
            return "ERR expected ambient|google-assistant|alexa\n";
        }
        std::string err;
        if (callbacks_.setNcButton && !callbacks_.setNcButton(button, err)) {
            return "ERR " + (err.empty() ? "failed to assign the NC button" : err) + "\n";
        }
        return "OK\n";
    }

    // 14. touch-panel <on|off>, voice-guidance <on|off>
    if (verb == "touch-panel" || verb == "voice-guidance") {
        const std::string val = tokens.size() >= 2 ? toLower(tokens[1]) : "";
        if (val != "on" && val != "off") {
            return "ERR expected on|off\n";
        }
        std::string err;
        const bool enabled = (val == "on");
        const bool ok = verb == "touch-panel"
            ? (!callbacks_.setTouchPanel || callbacks_.setTouchPanel(enabled, err))
            : (!callbacks_.setVoiceGuidance || callbacks_.setVoiceGuidance(enabled, err));
        if (!ok) {
            return "ERR " + (err.empty() ? "failed to set " + verb : err) + "\n";
        }
        return "OK\n";
    }

    // raw <hex byte> ... : send an arbitrary payload, for protocol exploration.
    // The headset's reply is logged by the daemon, not returned here.
    if (verb == "raw" || verb == "raw2") {
        if (tokens.size() < 2) {
            return "ERR expected hex bytes, e.g. raw 04 02\n";
        }
        std::vector<uint8_t> payload;
        for (size_t i = 1; i < tokens.size(); ++i) {
            char* end = nullptr;
            const long v = std::strtol(tokens[i].c_str(), &end, 16);
            if (end == tokens[i].c_str() || *end != '\0' || v < 0 || v > 0xFF) {
                return "ERR invalid hex byte '" + tokens[i] + "'\n";
            }
            payload.push_back(static_cast<uint8_t>(v));
        }
        if (callbacks_.sendPacket) {
            callbacks_.sendPacket(verb == "raw2" ? protocol::serializeRawT2(payload)
                                                 : protocol::serializeRaw(payload));
        }
        return "OK\n";
    }

    // Internal test helper verbs
    if (verb == "_set_battery") {
        int level = 85;
        bool charging = false;
        if (tokens.size() >= 2) {
            parseInt(tokens[1], level);
        }
        if (tokens.size() >= 3) {
            std::string chg = toLower(tokens[2]);
            charging = (chg == "true" || chg == "1" || chg == "yes");
        }
        if (callbacks_.onTestSetBattery) {
            callbacks_.onTestSetBattery(level, charging);
        }
        return "OK\n";
    }

    if (verb == "_disconnect") {
        if (callbacks_.onTestDisconnect) {
            callbacks_.onTestDisconnect();
        }
        return "OK\n";
    }

    if (verb == "_reconnect") {
        if (callbacks_.onTestReconnect) {
            callbacks_.onTestReconnect();
        }
        return "OK\n";
    }

    return "ERR unknown command '" + tokens[0] + "'\n";
}

} // namespace omarchy::sony
