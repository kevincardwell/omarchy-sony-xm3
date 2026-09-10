#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// The XM3 exposes noise control as a single 0..19 axis: step 0 is noise
// cancelling, step 1 is wind noise reduction, and 2..19 are ambient sound.
// The daemon narrows this to whatever the headset actually reports.
constexpr int MAX_AMBIENT_STEP = 19;
constexpr int MIN_AMBIENT_STEP = 2;

std::string to_lower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_int(const std::string& str, int& out) {
    if (str.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(str.c_str(), &end, 10);
    if (errno != 0 || end == str.c_str() || *end != '\0') {
        return false;
    }
    if (val < std::numeric_limits<int>::min() || val > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(val);
    return true;
}

bool is_negative_number(const std::string& str) {
    if (str.size() < 2 || str[0] != '-') {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(str[1])) != 0;
}

void print_usage(std::ostream& os) {
    os << "Usage: sony-xm3-ctl [-s <socket>] <subcommand> [args...]\n\n"
       << "Subcommands:\n"
       << "  status                     Print the full headset state as JSON\n"
       << "  noise <mode> [level]       anc | wind | ambient [2-" << MAX_AMBIENT_STEP << "] | off\n"
       << "  ambient-level <0-" << MAX_AMBIENT_STEP << ">       0 = ANC, 1 = wind reduction, 2+ = ambient\n"
       << "  voice-focus <on|off>       Focus on Voice (ambient step 2 and above)\n"
       << "  eq <preset>                off | bright | excited | mellow | relaxed |\n"
       << "                             vocal | treble | bass | speech | user1 | user2\n"
       << "  eq custom <b1..b5> <cb>    Five bands and Clear Bass, each -10..10\n"
       << "  dsee <on|off>              DSEE HX upscaling\n"
       << "  ear-detect <on|off>        Pause playback when removed\n"
       << "  surround <preset>          off | outdoor | arena | concert | club\n"
       << "  sound-position <pos>       off | front-left | front-right | front |\n"
       << "                             rear-left | rear-right\n"
       << "  auto-power-off <value>     off | 5min | 30min | 60min | 180min | on-remove\n"
       << "  connection <mode>          quality | stable\n\n"
       << "Options:\n"
       << "  -s, --socket <path>  Override socket path\n"
       << "  -h, --help           Show help\n"
       << "  -v, --version        Show version\n";
}

std::string get_default_socket_path() {
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != nullptr && *xdg_runtime != '\0') {
        return std::string(xdg_runtime) + "/sony-xm3.sock";
    }
    return "/tmp/run-" + std::to_string(::getuid()) + "/sony-xm3.sock";
}

int send_command(const std::string& socket_path, const std::string& command_str, bool is_status) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Error: Cannot create socket (" << std::strerror(errno) << ")\n";
        return 1;
    }

    // Configure 2.0 second socket timeouts
    struct timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "Error: Socket path too long: " << socket_path << "\n";
        ::close(fd);
        return 1;
    }
    std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());
    addr.sun_path[socket_path.size()] = '\0';

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Error: Cannot connect to daemon socket (" << std::strerror(errno) << ")\n";
        ::close(fd);
        return 2;
    }

    std::string payload = command_str;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    size_t total_sent = 0;
    while (total_sent < payload.size()) {
        ssize_t sent = ::send(fd, payload.data() + total_sent, payload.size() - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Socket communication failed (" << std::strerror(errno) << ")\n";
            ::close(fd);
            return 1;
        }
        if (sent == 0) {
            std::cerr << "Error: Socket communication failed (connection closed)\n";
            ::close(fd);
            return 1;
        }
        total_sent += static_cast<size_t>(sent);
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            response.append(buf, static_cast<size_t>(n));
            if (response.find('\n') != std::string::npos) {
                break;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Socket communication failed (" << std::strerror(errno) << ")\n";
            ::close(fd);
            return 1;
        }
    }
    ::close(fd);

    size_t newline_pos = response.find('\n');
    if (newline_pos != std::string::npos) {
        response = response.substr(0, newline_pos);
    }
    while (!response.empty() && (response.back() == '\r' || response.back() == ' ' || response.back() == '\t')) {
        response.pop_back();
    }

    if (response.empty()) {
        std::cerr << "Error: Empty response from daemon\n";
        return 1;
    }

    if (is_status) {
        if (response.rfind("ERR", 0) == 0) {
            std::cerr << "Error from daemon: " << response << "\n";
            return 1;
        }
        std::cout << response << "\n";
        return 0;
    }

    if (response.rfind("OK", 0) == 0) {
        std::cout << "OK\n";
        return 0;
    }

    std::cerr << "Error from daemon: " << response << "\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string socket_path;
    bool show_help = false;
    bool show_version = false;
    std::vector<std::string> remaining;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_help = true;
        } else if (arg == "-v" || arg == "--version") {
            show_version = true;
        } else if (arg == "-s" || arg == "--socket") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a socket path argument\n";
                return 1;
            }
            socket_path = argv[++i];
        } else if (arg.rfind("--socket=", 0) == 0) {
            socket_path = arg.substr(9);
        } else if (arg == "--") {
            for (++i; i < argc; ++i) {
                remaining.emplace_back(argv[i]);
            }
            break;
        } else if (!arg.empty() && arg[0] == '-' && !is_negative_number(arg) && remaining.empty()) {
            std::cerr << "Error: Unknown option '" << arg << "'\n";
            print_usage(std::cerr);
            return 1;
        } else {
            remaining.push_back(arg);
        }
    }

    if (show_help) {
        print_usage(std::cout);
        return 0;
    }

    if (show_version) {
        std::cout << "sony-xm3-ctl 0.1.0\n";
        return 0;
    }

    if (remaining.empty()) {
        print_usage(std::cerr);
        return 1;
    }

    if (socket_path.empty()) {
        socket_path = get_default_socket_path();
    }

    std::string subcmd = to_lower(remaining[0]);

    if (subcmd == "status") {
        return send_command(socket_path, "status\n", true);
    }

    if (subcmd == "noise") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'noise' requires a mode: anc, wind, ambient, off\n";
            return 1;
        }
        std::string mode = to_lower(remaining[1]);
        if (mode != "anc" && mode != "ambient" && mode != "wind" && mode != "off") {
            std::cerr << "Error: Invalid noise mode '" << remaining[1] << "'\n";
            return 1;
        }
        // `noise ambient <level>` is a convenience for picking the mode and the
        // passthrough step in one call.
        if (mode == "ambient" && remaining.size() >= 3) {
            int level = 0;
            if (!parse_int(remaining[2], level)) {
                std::cerr << "Error: Invalid integer '" << remaining[2] << "'\n";
                return 1;
            }
            if (level < MIN_AMBIENT_STEP || level > MAX_AMBIENT_STEP) {
                std::cerr << "Error: Ambient level " << level << " out of range ["
                          << MIN_AMBIENT_STEP << ", " << MAX_AMBIENT_STEP << "]\n";
                return 1;
            }
            return send_command(socket_path, "noise ambient " + std::to_string(level) + "\n", false);
        }
        return send_command(socket_path, "noise " + mode + "\n", false);
    }

    if (subcmd == "ambient-level") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'ambient-level' requires an integer between 0 and "
                      << MAX_AMBIENT_STEP << "\n";
            return 1;
        }
        int level = 0;
        if (!parse_int(remaining[1], level)) {
            std::cerr << "Error: Invalid integer '" << remaining[1] << "'\n";
            return 1;
        }
        if (level < 0 || level > MAX_AMBIENT_STEP) {
            std::cerr << "Error: Level " << level << " out of range [0, " << MAX_AMBIENT_STEP << "]\n";
            return 1;
        }
        return send_command(socket_path, "ambient-level " + std::to_string(level) + "\n", false);
    }

    if (subcmd == "eq") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'eq' requires a preset or 'custom' with 6 parameters\n";
            return 1;
        }
        std::string preset = to_lower(remaining[1]);
        if (preset == "custom" || preset == "manual") {
            if (remaining.size() < 8) {
                std::cerr << "Error: 'eq custom' requires 5 bands and clear bass (6 integers between -10 and 10)\n";
                return 1;
            }
            std::array<int, 5> bands{};
            for (size_t i = 0; i < 5; ++i) {
                int b = 0;
                if (!parse_int(remaining[2 + i], b)) {
                    std::cerr << "Error: EQ parameters must be valid integers\n";
                    return 1;
                }
                if (b < -10 || b > 10) {
                    std::cerr << "Error: Band values must be between -10 and 10\n";
                    return 1;
                }
                bands[i] = b;
            }
            int cb = 0;
            if (!parse_int(remaining[7], cb)) {
                std::cerr << "Error: EQ parameters must be valid integers\n";
                return 1;
            }
            if (cb < -10 || cb > 10) {
                std::cerr << "Error: Clear Bass must be between -10 and 10\n";
                return 1;
            }
            std::string cmd = "eq custom " + std::to_string(bands[0]) + " "
                                           + std::to_string(bands[1]) + " "
                                           + std::to_string(bands[2]) + " "
                                           + std::to_string(bands[3]) + " "
                                           + std::to_string(bands[4]) + " "
                                           + std::to_string(cb) + "\n";
            return send_command(socket_path, cmd, false);
        }

        static const std::vector<std::string> valid_presets = {
            "off", "bright", "excited", "mellow", "relaxed",
            "vocal", "treble", "bass", "speech", "user1", "user2"
        };
        if (std::find(valid_presets.begin(), valid_presets.end(), preset) == valid_presets.end()) {
            std::cerr << "Error: Unknown EQ preset '" << remaining[1] << "'\n";
            return 1;
        }
        return send_command(socket_path, "eq " + preset + "\n", false);
    }

    if (subcmd == "voice-focus" || subcmd == "dsee" ||
        subcmd == "ear-detect" || subcmd == "ear-detection") {
        if (remaining.size() < 2) {
            std::cerr << "Error: '" << remaining[0] << "' requires 'on' or 'off'\n";
            return 1;
        }
        std::string val = to_lower(remaining[1]);
        if (val != "on" && val != "off") {
            std::cerr << "Error: '" << remaining[0] << "' requires 'on' or 'off'\n";
            return 1;
        }
        std::string verb = (subcmd == "ear-detection") ? "ear-detect" : subcmd;
        return send_command(socket_path, verb + " " + val + "\n", false);
    }

    if (subcmd == "surround") {
        static const std::vector<std::string> valid = {"off", "outdoor", "arena", "concert", "club"};
        if (remaining.size() < 2 ||
            std::find(valid.begin(), valid.end(), to_lower(remaining[1])) == valid.end()) {
            std::cerr << "Error: 'surround' requires one of: off, outdoor, arena, concert, club\n";
            return 1;
        }
        return send_command(socket_path, "surround " + to_lower(remaining[1]) + "\n", false);
    }

    if (subcmd == "sound-position") {
        static const std::vector<std::string> valid = {
            "off", "front-left", "front-right", "front", "rear-left", "rear-right"
        };
        if (remaining.size() < 2 ||
            std::find(valid.begin(), valid.end(), to_lower(remaining[1])) == valid.end()) {
            std::cerr << "Error: 'sound-position' requires one of: off, front-left, front-right, "
                         "front, rear-left, rear-right\n";
            return 1;
        }
        return send_command(socket_path, "sound-position " + to_lower(remaining[1]) + "\n", false);
    }

    if (subcmd == "auto-power-off") {
        static const std::vector<std::string> valid = {
            "off", "5min", "30min", "60min", "180min", "on-remove"
        };
        if (remaining.size() < 2 ||
            std::find(valid.begin(), valid.end(), to_lower(remaining[1])) == valid.end()) {
            std::cerr << "Error: 'auto-power-off' requires one of: off, 5min, 30min, 60min, "
                         "180min, on-remove\n";
            return 1;
        }
        return send_command(socket_path, "auto-power-off " + to_lower(remaining[1]) + "\n", false);
    }

    if (subcmd == "connection") {
        static const std::vector<std::string> valid = {"quality", "stable"};
        if (remaining.size() < 2 ||
            std::find(valid.begin(), valid.end(), to_lower(remaining[1])) == valid.end()) {
            std::cerr << "Error: 'connection' requires one of: quality, stable\n";
            return 1;
        }
        return send_command(socket_path, "connection " + to_lower(remaining[1]) + "\n", false);
    }

    std::cerr << "Error: Unknown subcommand '" << remaining[0] << "'\n";
    return 1;
}
