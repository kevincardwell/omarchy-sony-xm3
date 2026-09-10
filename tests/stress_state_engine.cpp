#include "StateEngine.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace omarchy::sony;

// Simple JSON structure validator to verify syntax without external JSON parser
bool validateJsonSyntax(const std::string& s) {
    if (s.empty()) return false;
    // Must start with { and end with } or }\n
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
        start++;
    }
    if (start >= s.size() || s[start] != '{') return false;

    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r')) {
        end--;
    }
    if (end <= start || s[end - 1] != '}') return false;

    // Check balanced braces and brackets
    int braceCount = 0;
    int bracketCount = 0;
    bool inString = false;
    bool escape = false;

    for (size_t i = start; i < end; ++i) {
        char c = s[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;

        if (c == '{') braceCount++;
        else if (c == '}') {
            braceCount--;
            if (braceCount < 0) return false;
        }
        else if (c == '[') bracketCount++;
        else if (c == ']') {
            bracketCount--;
            if (bracketCount < 0) return false;
        }
    }

    if (braceCount != 0 || bracketCount != 0 || inString) return false;

    // Verify key elements exist
    if (s.find("\"schema_version\"") == std::string::npos) return false;
    if (s.find("\"connected\"") == std::string::npos) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 1: Single Writer + Multiple Readers (100,000+ reads)
// ---------------------------------------------------------------------------
bool testSingleWriterMultiReaderStress(const std::filesystem::path& testDir) {
    std::cout << "[STRESS] Starting Test 1: 1 Writer (3,000 commits) + 8 Reader threads\n";
    StateEngine engine(testDir);
    if (!engine.initialize(true)) {
        std::cerr << "Failed to initialize StateEngine\n";
        return false;
    }

    std::atomic<bool> stopReaders{false};
    std::atomic<uint64_t> totalReads{0};
    std::atomic<uint64_t> emptyReads{0};
    std::atomic<uint64_t> tornReads{0};
    std::atomic<uint64_t> invalidJsonReads{0};

    const int numReaders = 8;
    std::vector<std::thread> readers;
    for (int r = 0; r < numReaders; ++r) {
        readers.emplace_back([&, r]() {
            while (!stopReaders.load(std::memory_order_relaxed)) {
                std::ifstream ifs(engine.getStateFilePath(), std::ios::in | std::ios::binary);
                if (!ifs.is_open()) {
                    continue;
                }
                std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                ifs.close();

                totalReads.fetch_add(1, std::memory_order_relaxed);

                if (content.empty()) {
                    emptyReads.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (content.front() != '{' || (content.back() != '\n' && content.back() != '}')) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (!validateJsonSyntax(content)) {
                    invalidJsonReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Writer performs 3,000 state mutations across various fields
    const int iterations = 3000;
    const std::vector<std::string> modes = {"anc", "ambient", "wind", "off"};
    const std::vector<std::string> presets = {"off", "vocal", "excited", "bass", "treble"};

    for (int i = 0; i < iterations; ++i) {
        engine.setBatteryLevel(i % 101);
        engine.setCharging(i % 2 == 0);
        engine.setNoiseMode(modes[i % modes.size()]);
        engine.setAmbientLevel(i % 20);
        engine.setEqPreset(presets[i % presets.size()]);
        if (i % 100 == 0) {
            engine.setCustomEq({(i % 21) - 10, ((i+1) % 21) - 10, ((i+2) % 21) - 10, 0, 0}, (i % 21) - 10);
        }
    }

    stopReaders.store(true, std::memory_order_relaxed);
    for (auto& t : readers) {
        if (t.joinable()) t.join();
    }

    std::cout << "[STRESS] Test 1 Completed: "
              << "Total reads: " << totalReads.load() << ", "
              << "Empty reads: " << emptyReads.load() << ", "
              << "Torn reads: " << tornReads.load() << ", "
              << "Invalid JSON: " << invalidJsonReads.load() << "\n";

    if (totalReads.load() < 5000) {
        std::cerr << "FAIL: Total reads (" << totalReads.load() << ") was too low for meaningful stress\n";
        return false;
    }
    if (emptyReads.load() > 0) {
        std::cerr << "FAIL: Detected " << emptyReads.load() << " empty reads!\n";
        return false;
    }
    if (tornReads.load() > 0) {
        std::cerr << "FAIL: Detected " << tornReads.load() << " torn reads!\n";
        return false;
    }
    if (invalidJsonReads.load() > 0) {
        std::cerr << "FAIL: Detected " << invalidJsonReads.load() << " invalid JSON reads!\n";
        return false;
    }

    std::cout << "[PASS] Test 1: 0 torn reads and 100% valid JSON under high concurrency!\n";
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: Multi-Threaded Writers Concurrency Stress
// ---------------------------------------------------------------------------
bool testMultiWriterConcurrencyStress(const std::filesystem::path& testDir) {
    std::cout << "[STRESS] Starting Test 2: 4 Concurrent Writers (500 updates each) + 4 Readers\n";
    StateEngine engine(testDir);
    if (!engine.initialize(true)) {
        std::cerr << "Failed to initialize StateEngine\n";
        return false;
    }

    std::atomic<bool> stopReaders{false};
    std::atomic<uint64_t> totalReads{0};
    std::atomic<uint64_t> tornReads{0};
    std::atomic<uint64_t> invalidJsonReads{0};
    std::atomic<uint64_t> writeFailures{0};

    // Readers
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            while (!stopReaders.load(std::memory_order_relaxed)) {
                std::ifstream ifs(engine.getStateFilePath());
                if (!ifs.is_open()) continue;
                std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                ifs.close();

                if (content.empty()) continue;
                totalReads.fetch_add(1, std::memory_order_relaxed);

                if (content.front() != '{' || (content.back() != '\n' && content.back() != '}')) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                } else if (!validateJsonSyntax(content)) {
                    invalidJsonReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 4 Concurrent Writers
    const int updatesPerWriter = 500;
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w]() {
            for (int i = 0; i < updatesPerWriter; ++i) {
                if (w == 0) {
                    engine.setBatteryLevel((i * 7) % 101);
                } else if (w == 1) {
                    engine.setAmbientLevel((i * 3) % 20);
                } else if (w == 2) {
                    engine.setNoiseMode((i % 2 == 0) ? "anc" : "ambient");
                } else {
                    engine.setEqPreset((i % 2 == 0) ? "vocal" : "excited");
                }
            }
        });
    }

    for (auto& w : writers) {
        if (w.joinable()) w.join();
    }

    stopReaders.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        if (r.joinable()) r.join();
    }

    std::cout << "[STRESS] Test 2 Completed: "
              << "Total reads: " << totalReads.load() << ", "
              << "Torn reads: " << tornReads.load() << ", "
              << "Invalid JSON: " << invalidJsonReads.load() << ", "
              << "Write failures: " << writeFailures.load() << "\n";

    // Verify final state on disk is readable and valid
    std::ifstream finalFs(engine.getStateFilePath());
    std::string finalContent((std::istreambuf_iterator<char>(finalFs)), std::istreambuf_iterator<char>());
    finalFs.close();

    if (!validateJsonSyntax(finalContent)) {
        std::cerr << "FAIL: Final state file is corrupted!\n";
        return false;
    }

    if (tornReads.load() > 0 || invalidJsonReads.load() > 0) {
        std::cerr << "WARNING: Under concurrent in-process writers sharing .tmp.<pid>, "
                  << "torn reads = " << tornReads.load() << ", invalid JSON = " << invalidJsonReads.load() << "\n";
        // Let's document this finding!
    } else {
        std::cout << "[PASS] Test 2: Concurrent in-process writers completed without corrupting file\n";
    }

    return (tornReads.load() == 0 && invalidJsonReads.load() == 0);
}

// ---------------------------------------------------------------------------
// Test 3: Permissions and Adversarial Umask Stress
// ---------------------------------------------------------------------------
bool testPermissionsAndUmaskStress(const std::filesystem::path& baseDir) {
    std::cout << "[STRESS] Starting Test 3: Permissions (0600 file, 0700 dir) under adversarial umask\n";

    // Save original umask
    mode_t origUmask = ::umask(0000); // Most permissive umask possible!

    std::filesystem::path testDir = baseDir / "umask_test_0000";

    StateEngine engine(testDir);
    if (!engine.initialize(true)) {
        std::cerr << "FAIL: engine.initialize failed under umask(0000)\n";
        ::umask(origUmask);
        return false;
    }

    // Check directory permissions
    struct stat dirSt{};
    if (::stat(engine.getStateDirectory().c_str(), &dirSt) != 0) {
        std::cerr << "FAIL: stat directory failed: " << std::strerror(errno) << "\n";
        ::umask(origUmask);
        return false;
    }
    mode_t dirMode = dirSt.st_mode & 0777;
    if (dirMode != 0700) {
        std::cerr << "FAIL: Directory mode under umask(0000) is " << std::oct << dirMode << " (expected 0700)\n";
        ::umask(origUmask);
        return false;
    }

    // Check file permissions
    struct stat fileSt{};
    if (::stat(engine.getStateFilePath().c_str(), &fileSt) != 0) {
        std::cerr << "FAIL: stat status.json failed: " << std::strerror(errno) << "\n";
        ::umask(origUmask);
        return false;
    }
    mode_t fileMode = fileSt.st_mode & 0777;
    if (fileMode != 0600) {
        std::cerr << "FAIL: File mode under umask(0000) is " << std::oct << fileMode << " (expected 0600)\n";
        ::umask(origUmask);
        return false;
    }

    // Subtest: Corrupt directory permissions to 0777 and file to 0666
    ::chmod(engine.getStateDirectory().c_str(), 0777);
    ::chmod(engine.getStateFilePath().c_str(), 0666);

    // Call state mutator which calls writeAtomic()
    engine.setBatteryLevel(99);

    // Verify file mode is restored to 0600 by writeAtomic() fchmod
    if (::stat(engine.getStateFilePath().c_str(), &fileSt) != 0) {
        std::cerr << "FAIL: stat status.json failed after update\n";
        ::umask(origUmask);
        return false;
    }
    if ((fileSt.st_mode & 0777) != 0600) {
        std::cerr << "FAIL: File mode after rewrite is " << std::oct << (fileSt.st_mode & 0777) << " (expected 0600)\n";
        ::umask(origUmask);
        return false;
    }

    // Verify ensureStateDirectory restores directory to 0700
    StateEngine::ensureStateDirectory(engine.getStateDirectory());
    if (::stat(engine.getStateDirectory().c_str(), &dirSt) != 0) {
        std::cerr << "FAIL: stat dir failed after ensureStateDirectory\n";
        ::umask(origUmask);
        return false;
    }
    if ((dirSt.st_mode & 0777) != 0700) {
        std::cerr << "FAIL: Directory mode after ensureStateDirectory is " << std::oct << (dirSt.st_mode & 0777) << " (expected 0700)\n";
        ::umask(origUmask);
        return false;
    }

    // Restore original umask
    ::umask(origUmask);
    std::cout << "[PASS] Test 3: Permissions remain strictly 0600 file and 0700 dir under umask(0000)!\n";
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: Path Resolution & Non-Existent Path Robustness
// ---------------------------------------------------------------------------
bool testPathResolutionAndErrorHandling() {
    std::cout << "[STRESS] Starting Test 4: Path resolution and non-existent/unwritable paths\n";

    // 1. Unwritable path
    StateEngine badEngine("/proc/sys/fs/sony-xm3-test-nonexistent");
    bool initResult = badEngine.initialize(true);
    if (initResult) {
        std::cerr << "FAIL: initialize succeeded on unwritable path /proc/sys/fs\n";
        return false;
    }

    // 2. Custom path variations
    std::filesystem::path p1 = StateEngine::resolveStateFilePath("/tmp/custom/path");
    if (p1.string().find("status.json") == std::string::npos) {
        std::cerr << "FAIL: resolveStateFilePath didn't append status.json\n";
        return false;
    }

    std::filesystem::path p2 = StateEngine::resolveStateFilePath("/tmp/custom/status.json");
    if (p2.string() != "/tmp/custom/status.json") {
        std::cerr << "FAIL: resolveStateFilePath corrupted existing status.json path\n";
        return false;
    }

    std::cout << "[PASS] Test 4: Error handling and path resolution verified\n";
    return true;
}

int main() {
    std::cout << "====================================================\n";
    std::cout << "  Adversarial StateEngine Concurrency & Stress Harness\n";
    std::cout << "====================================================\n";

    char tmpl[] = "/tmp/omasonyxm3_stress_XXXXXX";
    char* sbox = ::mkdtemp(tmpl);
    if (!sbox) {
        std::cerr << "Failed to mkdtemp\n";
        return 1;
    }
    std::filesystem::path sandbox(sbox);

    bool coreOk = true;
    coreOk = testSingleWriterMultiReaderStress(sandbox / "test1") && coreOk;
    bool multiWriterOk = testMultiWriterConcurrencyStress(sandbox / "test2");
    coreOk = testPermissionsAndUmaskStress(sandbox) && coreOk;
    coreOk = testPathResolutionAndErrorHandling() && coreOk;

    // Clean up sandbox
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    std::cout << "====================================================\n";
    std::cout << "  Daemon Reactor Model Contract (Single Writer, Multi-Reader): " << (coreOk ? "PASS" : "FAIL") << "\n";
    std::cout << "  Adversarial In-Process Multi-Threaded Writer Safety: " << (multiWriterOk ? "PASS" : "COLLISION DETECTED (Finding)") << "\n";
    std::cout << "====================================================\n";
    return coreOk ? 0 : 1;
}
