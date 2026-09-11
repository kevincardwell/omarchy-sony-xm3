// installer/deploy.cpp
// sony-xm3-deploy: puts files into, and removes named files from, directories
// under the invoking user's home directory without following symbolic links.
//
// setup uses this instead of cp, install and rm so that a symlinked
// destination can never redirect a write or a deletion somewhere else:
//
//   - The home directory comes from the password database, not $HOME.
//   - Every directory below it is opened relative to its parent with
//     O_NOFOLLOW | O_DIRECTORY, checked (owned by the caller, not
//     world-writable), and the descriptor is kept for everything that follows.
//   - Files are written to an O_EXCL | O_NOFOLLOW temporary in that directory
//     and renamed over the target, which replaces a symlink rather than
//     writing through it.
//   - Removal unlinks only the entries it is given, by name, relative to the
//     retained descriptor. Nothing is ever deleted recursively.
//
// Usage:
//   sony-xm3-deploy put [--unless-git-checkout] <dir> <mode> <file>...
//   sony-xm3-deploy remove [--rmdir] <dir> <name>...
//
// <dir> is relative to the home directory. A <name> ending in '*' matches
// every entry with that prefix. `--home <dir>` (first argument) substitutes an
// absolute directory for the home directory; the test suite uses it.
//
// Exit status: 0 on success, 1 on error, 3 when --unless-git-checkout found a
// git checkout that is not the source.

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kExitManagedCheckout = 3;

[[noreturn]] void die(const std::string& message) {
    std::cerr << "sony-xm3-deploy: " << message << "\n";
    std::exit(1);
}

std::string errnoText() { return std::strerror(errno); }

// Owns one file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

std::string g_homeOverride;

std::string homeDirectory() {
    if (!g_homeOverride.empty()) return g_homeOverride;
    const passwd* pw = ::getpwuid(::getuid());
    if (pw == nullptr || pw->pw_dir == nullptr || pw->pw_dir[0] != '/') {
        die("cannot determine your home directory from the password database");
    }
    return pw->pw_dir;
}

bool isPlainName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos;
}

std::vector<std::string> splitRelative(const std::string& relative) {
    if (relative.empty() || relative.front() == '/') {
        die("destination must be relative to the home directory: '" + relative + "'");
    }
    std::vector<std::string> parts;
    std::string::size_type start = 0;
    while (start <= relative.size()) {
        const auto slash = relative.find('/', start);
        const auto end = slash == std::string::npos ? relative.size() : slash;
        std::string part = relative.substr(start, end - start);
        if (!isPlainName(part)) die("invalid path component in '" + relative + "'");
        parts.push_back(std::move(part));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return parts;
}

// Refuses a directory that someone else could have prepared or can change.
void checkDirectory(int fd, const std::string& shown) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) die("cannot stat " + shown + ": " + errnoText());
    if (!S_ISDIR(st.st_mode)) die(shown + " is not a directory");
    if (st.st_uid != ::getuid()) die(shown + " is not owned by you; refusing to use it");
    if ((st.st_mode & S_IWOTH) != 0) die(shown + " is writable by other users; refusing to use it");
}

struct Walk {
    Fd dir;              // the destination directory, when found
    Fd parent;           // its parent, kept so it can be removed
    std::string shown;   // path for messages only; never used for access
    std::string last;    // final component of the destination
    bool found = false;
};

// Opens `relative` below the home directory one component at a time. No
// component is ever followed if it is a symlink. With `create`, missing
// directories are made; otherwise a missing one ends the walk with found=false.
Walk walkFromHome(const std::string& relative, bool create) {
    const std::string home = homeDirectory();
    Fd current(::open(home.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current.valid()) die("cannot open " + home + ": " + errnoText());
    checkDirectory(current.get(), home);

    Walk walk;
    walk.shown = home;
    const auto parts = splitRelative(relative);
    for (const auto& part : parts) {
        walk.shown += "/" + part;
        const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        int fd = ::openat(current.get(), part.c_str(), flags);
        if (fd < 0 && errno == ENOENT && create) {
            if (::mkdirat(current.get(), part.c_str(), 0755) != 0 && errno != EEXIST) {
                die("cannot create " + walk.shown + ": " + errnoText());
            }
            // Reopened with O_NOFOLLOW, so a symlink raced into place is refused.
            fd = ::openat(current.get(), part.c_str(), flags);
        }
        if (fd < 0) {
            if (errno == ENOENT) {
                walk.last = part;
                return walk;
            }
            if (errno == ELOOP || errno == ENOTDIR) {
                die(walk.shown + " is a symbolic link or not a directory; refusing to follow it");
            }
            die("cannot open " + walk.shown + ": " + errnoText());
        }
        walk.parent = std::move(current);
        current = Fd(fd);
        checkDirectory(current.get(), walk.shown);
    }
    walk.dir = std::move(current);
    walk.last = parts.back();
    walk.found = true;
    return walk;
}

struct Source {
    std::string path;
    std::string name;
    Fd fd;
    struct stat st {};
};

void copyAll(int from, int to, const std::string& what) {
    if (::lseek(from, 0, SEEK_SET) != 0) die("cannot rewind " + what + ": " + errnoText());
    char buffer[1 << 16];
    for (;;) {
        const ssize_t got = ::read(from, buffer, sizeof buffer);
        if (got == 0) return;
        if (got < 0) {
            if (errno == EINTR) continue;
            die("cannot read " + what + ": " + errnoText());
        }
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = ::write(to, buffer + done, static_cast<size_t>(got - done));
            if (put < 0) {
                if (errno == EINTR) continue;
                die("cannot write " + what + ": " + errnoText());
            }
            done += put;
        }
    }
}

// Writes `source` into `dir` as a new inode and renames it over the target.
void placeFile(const Walk& walk, const Source& source, mode_t mode) {
    const int dir = walk.dir.get();
    const std::string target = walk.shown + "/" + source.name;

    struct stat existing {};
    if (::fstatat(dir, source.name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(existing.st_mode)) {
        die(target + " is a directory; refusing to replace it");
    }

    const std::string temp = "." + source.name + ".deploy." + std::to_string(::getpid());
    Fd out(::openat(dir, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!out.valid()) die("cannot create a temporary file in " + walk.shown + ": " + errnoText());

    auto fail = [&](const std::string& message) {
        ::unlinkat(dir, temp.c_str(), 0);
        die(message);
    };
    copyAll(source.fd.get(), out.get(), source.path);
    if (::fchmod(out.get(), mode) != 0) fail("cannot set the mode of " + target + ": " + errnoText());
    if (::fsync(out.get()) != 0) fail("cannot flush " + target + ": " + errnoText());
    out.reset();
    if (::renameat(dir, temp.c_str(), dir, source.name.c_str()) != 0) {
        fail("cannot move " + target + " into place: " + errnoText());
    }
    std::cout << "installed " << target << "\n";
}

int commandPut(std::vector<std::string> args) {
    bool unlessGitCheckout = false;
    if (!args.empty() && args.front() == "--unless-git-checkout") {
        unlessGitCheckout = true;
        args.erase(args.begin());
    }
    if (args.size() < 3) die("usage: put [--unless-git-checkout] <dir> <mode> <file>...");

    const std::string destination = args[0];
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(args[1].c_str(), &end, 8);
    if (end == args[1].c_str() || *end != '\0' || parsed > 0777) die("invalid mode '" + args[1] + "'");
    const auto mode = static_cast<mode_t>(parsed);

    std::vector<Source> sources;
    for (auto it = args.begin() + 2; it != args.end(); ++it) {
        Source source;
        source.path = *it;
        const auto slash = it->rfind('/');
        source.name = slash == std::string::npos ? *it : it->substr(slash + 1);
        if (!isPlainName(source.name)) die("invalid file name '" + *it + "'");
        source.fd = Fd(::open(it->c_str(), O_RDONLY | O_CLOEXEC));
        if (!source.fd.valid()) die("cannot open " + *it + ": " + errnoText());
        if (::fstat(source.fd.get(), &source.st) != 0 || !S_ISREG(source.st.st_mode)) {
            die(*it + " is not a regular file");
        }
        sources.push_back(std::move(source));
    }

    const Walk walk = walkFromHome(destination, /*create=*/true);

    // Running from the installed copy itself: every target already is its source.
    bool allInPlace = true;
    for (const auto& source : sources) {
        struct stat existing {};
        const bool same = ::fstatat(walk.dir.get(), source.name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 &&
                          existing.st_dev == source.st.st_dev && existing.st_ino == source.st.st_ino;
        if (!same) allInPlace = false;
    }
    if (allInPlace) {
        std::cout << walk.shown << " already holds these files; nothing to copy\n";
        return 0;
    }

    if (unlessGitCheckout) {
        struct stat git {};
        if (::fstatat(walk.dir.get(), ".git", &git, AT_SYMLINK_NOFOLLOW) == 0) {
            std::cout << walk.shown << " is a git checkout; left unchanged\n";
            return kExitManagedCheckout;
        }
    }

    for (const auto& source : sources) placeFile(walk, source, mode);
    return 0;
}

bool hasPrefix(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

// Expands names ending in '*' against the directory's entries.
std::vector<std::string> expandNames(int dir, const std::vector<std::string>& patterns, const std::string& shown) {
    std::vector<std::string> names;
    std::vector<std::string> prefixes;
    for (const auto& pattern : patterns) {
        if (!pattern.empty() && pattern.back() == '*') {
            const std::string prefix = pattern.substr(0, pattern.size() - 1);
            if (!isPlainName(prefix)) die("invalid pattern '" + pattern + "'");
            prefixes.push_back(prefix);
        } else {
            if (!isPlainName(pattern)) die("invalid name '" + pattern + "'");
            names.push_back(pattern);
        }
    }
    if (prefixes.empty()) return names;

    const int copy = ::dup(dir);
    if (copy < 0) die("cannot read " + shown + ": " + errnoText());
    DIR* listing = ::fdopendir(copy);
    if (listing == nullptr) {
        ::close(copy);
        die("cannot read " + shown + ": " + errnoText());
    }
    ::rewinddir(listing);
    while (const dirent* entry = ::readdir(listing)) {
        const std::string name = entry->d_name;
        if (!isPlainName(name)) continue;
        for (const auto& prefix : prefixes) {
            if (hasPrefix(name, prefix)) {
                names.push_back(name);
                break;
            }
        }
    }
    ::closedir(listing);
    return names;
}

int commandRemove(std::vector<std::string> args) {
    bool removeDirectory = false;
    if (!args.empty() && args.front() == "--rmdir") {
        removeDirectory = true;
        args.erase(args.begin());
    }
    if (args.size() < 2) die("usage: remove [--rmdir] <dir> <name>...");

    const Walk walk = walkFromHome(args[0], /*create=*/false);
    if (!walk.found) {
        std::cout << walk.shown << " is not present\n";
        return 0;
    }

    const int dir = walk.dir.get();
    const std::vector<std::string> patterns(args.begin() + 1, args.end());
    for (const auto& name : expandNames(dir, patterns, walk.shown)) {
        const std::string target = walk.shown + "/" + name;
        struct stat st {};
        if (::fstatat(dir, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            die("cannot stat " + target + ": " + errnoText());
        }
        if (S_ISDIR(st.st_mode)) {
            std::cout << "left " << target << ": it is a directory\n";
            continue;
        }
        // Unlinks the entry itself; a symlink is removed, never its target.
        if (::unlinkat(dir, name.c_str(), 0) != 0) die("cannot remove " + target + ": " + errnoText());
        std::cout << "removed " << target << "\n";
    }

    if (removeDirectory && walk.parent.valid()) {
        if (::unlinkat(walk.parent.get(), walk.last.c_str(), AT_REMOVEDIR) == 0) {
            std::cout << "removed " << walk.shown << "\n";
        } else if (errno == ENOTEMPTY || errno == EEXIST) {
            std::cout << "left " << walk.shown << ": it still holds other files\n";
        } else {
            die("cannot remove " + walk.shown + ": " + errnoText());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() >= 2 && args[0] == "--home") {
        if (args[1].empty() || args[1].front() != '/') die("--home needs an absolute directory");
        g_homeOverride = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty()) die("usage: sony-xm3-deploy put|remove ...");

    const std::string command = args.front();
    args.erase(args.begin());
    if (command == "put") return commandPut(std::move(args));
    if (command == "remove") return commandRemove(std::move(args));
    die("unknown command '" + command + "'");
}
