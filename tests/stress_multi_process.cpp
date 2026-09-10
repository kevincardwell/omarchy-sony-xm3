#include "StateEngine.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>

using namespace omarchy::sony;

int main() {
    char tmpl[] = "/tmp/omasonyxm3_multiproc_XXXXXX";
    char* sbox = ::mkdtemp(tmpl);
    if (!sbox) return 1;
    std::filesystem::path sandbox(sbox);

    std::cout << "[STRESS] Starting Multi-Process Concurrency Test (4 processes writing concurrently)\n";

    // Initialize StateEngine in parent
    {
        StateEngine engine(sandbox);
        if (!engine.initialize(true)) {
            std::cerr << "Init failed\n";
            return 1;
        }
    }

    const int numChildren = 4;
    pid_t pids[numChildren];

    for (int c = 0; c < numChildren; ++c) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            StateEngine childEngine(sandbox);
            for (int i = 0; i < 200; ++i) {
                childEngine.setBatteryLevel((c * 25 + i) % 101);
                childEngine.setAmbientLevel(i % 20);
                usleep(500);
            }
            _exit(0);
        } else if (pid > 0) {
            pids[c] = pid;
        } else {
            std::cerr << "Fork failed\n";
            return 1;
        }
    }

    // Parent concurrently reads status.json
    std::string stateFile = (sandbox / "sony-xm3" / "status.json").string();
    std::atomic<bool> done{false};
    uint64_t totalReads = 0;
    uint64_t tornReads = 0;

    std::thread reader([&]() {
        while (!done.load()) {
            std::ifstream ifs(stateFile);
            if (!ifs.is_open()) continue;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.empty()) continue;
            totalReads++;
            if (content.front() != '{' || (content.back() != '\n' && content.back() != '}')) {
                tornReads++;
            }
        }
    });

    // Wait for all children
    for (int c = 0; c < numChildren; ++c) {
        int status = 0;
        waitpid(pids[c], &status, 0);
    }
    done = true;
    reader.join();

    std::cout << "[STRESS] Multi-process test: Total reads=" << totalReads << ", Torn reads=" << tornReads << "\n";

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    if (tornReads == 0 && totalReads > 500) {
        std::cout << "[PASS] Multi-process atomic persistence verified: 0 torn reads across independent processes!\n";
        return 0;
    } else {
        std::cerr << "FAIL: Multi-process test failed (torn=" << tornReads << ", total=" << totalReads << ")\n";
        return 1;
    }
}
