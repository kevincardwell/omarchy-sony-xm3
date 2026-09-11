#!/usr/bin/env python3
"""
Adversarial Stress Harness for Milestone 2 Iteration 2
OMARCHY-SONY Empirical Challenger

Tests:
1. Rapid Status Queries & Concurrent Socket Reads (Compact JSON verification)
2. StateEngine Atomic File Updates Under Continuous Concurrent Readers (Mode 0600 check)
3. Abrupt Client Disconnections & Pipelining Stress (UAF / crash check)
4. Clean Shutdown (SIGTERM, SIGINT) & Stale Socket Recovery
5. Pipelined Mixed Valid/Invalid Commands & Sequential Synchronization
"""

import os
import sys
import time
import signal
import socket
import tempfile
import subprocess
import threading
import json
import stat
import random

# Resolved relative to the repository so the suite is not tied to one checkout.
DAEMON_BIN = os.environ.get("DAEMON_BIN", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "daemon", "sony-xm3-daemon"))

def check_mode(path, expected_mode):
    st = os.stat(path)
    actual_mode = stat.S_IMODE(st.st_mode)
    return actual_mode == expected_mode, actual_mode

class DaemonContext:
    def __init__(self, sbox, name):
        self.sbox = sbox
        self.state_dir = os.path.join(sbox, f"state_{name}")
        self.run_dir = os.path.join(sbox, f"run_{name}")
        self.sock_path = os.path.join(self.run_dir, "sony-xm3.sock")
        self.status_path = os.path.join(self.state_dir, "sony-xm3", "status.json")
        self.proc = None

    def start(self):
        os.makedirs(self.state_dir, exist_ok=True)
        os.makedirs(self.run_dir, exist_ok=True)
        self.proc = subprocess.Popen(
            [DAEMON_BIN, "--mock", "--state-dir", self.state_dir, "--runtime-dir", self.run_dir],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        # Wait for ready line
        start_t = time.time()
        ready = False
        while time.time() - start_t < 4.0:
            line = self.proc.stdout.readline()
            if "[MOCK_DAEMON] Ready" in line:
                ready = True
                break
            time.sleep(0.01)
        if not ready:
            self.stop_kill()
            raise RuntimeError(f"Daemon did not start in time. Stderr: {self.proc.stderr.read()}")

        # Ensure socket exists
        if not os.path.exists(self.sock_path):
            raise RuntimeError(f"Socket {self.sock_path} missing after start")

    def stop_term(self, timeout=3.0):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                ret = self.proc.wait(timeout=timeout)
                return ret
            except subprocess.TimeoutExpired:
                self.proc.kill()
                return -1
        return 0

    def stop_int(self, timeout=3.0):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                ret = self.proc.wait(timeout=timeout)
                return ret
            except subprocess.TimeoutExpired:
                self.proc.kill()
                return -1
        return 0

    def stop_kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()

def send_cmd(sock_path, cmd, timeout=2.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall(cmd.encode("utf-8"))
    res = b""
    while not res.endswith(b"\n"):
        chunk = s.recv(2048)
        if not chunk:
            break
        res += chunk
    s.close()
    return res.decode("utf-8")

# ---------------------------------------------------------------------------
# Test 1: Rapid Concurrent Status Queries (Compact JSON verification)
# ---------------------------------------------------------------------------
def test_rapid_concurrent_status(sbox):
    print("=== [CHALLENGE 1] Rapid Concurrent Status Queries (Compact JSON) ===")
    ctx = DaemonContext(sbox, "status_stress")
    ctx.start()

    errors = []
    total_queries = 0
    lock = threading.Lock()

    def worker(worker_id, num_queries):
        nonlocal total_queries
        for i in range(num_queries):
            try:
                resp = send_cmd(ctx.sock_path, "status\n", timeout=2.0)
                with lock:
                    total_queries += 1

                # 1. Must be single line terminating in \n
                if not resp.endswith("\n"):
                    errors.append(f"Worker {worker_id} query {i}: response does not end with \\n")
                    continue
                trimmed = resp[:-1]
                if "\n" in trimmed:
                    errors.append(f"Worker {worker_id} query {i}: response contains embedded newline (not compact)")
                    continue

                # 2. Strict JSON validation
                data = json.loads(trimmed)
                if data.get("schema_version") != 1:
                    errors.append(f"Worker {worker_id} query {i}: schema_version != 1")
                if not data.get("connected"):
                    errors.append(f"Worker {worker_id} query {i}: connected is not True")
                if not isinstance(data.get("battery_level"), int):
                    errors.append(f"Worker {worker_id} query {i}: battery_level is not int")
                if not isinstance(data.get("eq_bands"), list) or len(data["eq_bands"]) != 5:
                    errors.append(f"Worker {worker_id} query {i}: eq_bands invalid")
            except Exception as e:
                errors.append(f"Worker {worker_id} query {i}: Exception {e}")

    threads = []
    num_workers = 30
    queries_per_worker = 50
    for w in range(num_workers):
        t = threading.Thread(target=worker, args=(w, queries_per_worker))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    ret = ctx.stop_term()
    print(f"Completed {total_queries} status queries across {num_workers} concurrent clients.")
    if errors:
        print(f"FAILED: {len(errors)} errors detected:")
        for err in errors[:10]:
            print(f"  - {err}")
        return False

    if ret != 0:
        print(f"FAILED: Daemon exited with code {ret}")
        return False

    print("[PASS] Challenge 1: 1500 concurrent status queries verified compact JSON with 0 errors.")
    return True

# ---------------------------------------------------------------------------
# Test 2: StateEngine Atomic File Updates Under Continuous Readers
# ---------------------------------------------------------------------------
def test_atomic_file_updates_under_load(sbox):
    print("=== [CHALLENGE 2] Atomic Updates & Mode 0600 Under Continuous Readers ===")
    ctx = DaemonContext(sbox, "atomic_stress")
    ctx.start()

    stop_flag = False
    read_errors = []
    torn_reads = 0
    empty_reads = 0
    bad_permissions = 0
    total_reads = 0
    total_mutations = 0

    def file_reader():
        nonlocal torn_reads, empty_reads, bad_permissions, total_reads
        while not stop_flag:
            try:
                if os.path.exists(ctx.status_path):
                    # Check file mode
                    st = os.stat(ctx.status_path)
                    mode = stat.S_IMODE(st.st_mode)
                    if mode != 0o600:
                        bad_permissions += 1

                    # Check parent dir mode
                    parent_st = os.stat(os.path.dirname(ctx.status_path))
                    parent_mode = stat.S_IMODE(parent_st.st_mode)
                    if parent_mode != 0o700:
                        bad_permissions += 1

                    with open(ctx.status_path, "r") as f:
                        content = f.read()

                    if len(content) == 0:
                        empty_reads += 1
                        continue

                    total_reads += 1
                    # Parse JSON
                    data = json.loads(content)
                    if "schema_version" not in data or "connected" not in data:
                        torn_reads += 1
            except Exception as e:
                torn_reads += 1
            time.sleep(0.0005)

    def mutator(worker_id, count):
        nonlocal total_mutations
        cmds = [
            "noise anc\n",
            "noise ambient\n",
            "noise wind\n",
            "ambient-level 5\n",
            "ambient-level 18\n",
            "eq vocal\n",
            "eq custom 1 2 3 4 5 0\n",
            "voice-focus on\n",
            "voice-focus off\n",
            "dsee on\n",
            "dsee off\n",
            "surround arena\n",
            "dsee on\n",
            "_set_battery 90 false\n",
            "_set_battery 20 true\n",
        ]
        for i in range(count):
            cmd = random.choice(cmds)
            try:
                resp = send_cmd(ctx.sock_path, cmd, timeout=2.0)
                if not resp.startswith("OK"):
                    read_errors.append(f"Mutator {worker_id} command failed: {cmd} -> {resp}")
                total_mutations += 1
            except Exception as e:
                read_errors.append(f"Mutator {worker_id} exception: {e}")

    # Launch 6 continuous reader threads
    reader_threads = [threading.Thread(target=file_reader) for _ in range(6)]
    for rt in reader_threads:
        rt.start()

    # Launch 4 mutator threads
    mutator_threads = [threading.Thread(target=mutator, args=(m, 100)) for m in range(4)]
    for mt in mutator_threads:
        mt.start()

    for mt in mutator_threads:
        mt.join()

    # Allow readers a brief moment, then stop
    time.sleep(0.2)
    stop_flag = True
    for rt in reader_threads:
        rt.join()

    ret = ctx.stop_term()

    print(f"Total mutations: {total_mutations}, Total file reads: {total_reads}")
    print(f"Empty reads: {empty_reads}, Torn reads: {torn_reads}, Bad permissions: {bad_permissions}")

    if read_errors:
        print(f"FAILED: Mutator errors: {read_errors[:5]}")
        return False
    if empty_reads > 0:
        print(f"FAILED: Detected {empty_reads} empty reads (atomicity violated)!")
        return False
    if torn_reads > 0:
        print(f"FAILED: Detected {torn_reads} torn reads!")
        return False
    if bad_permissions > 0:
        print(f"FAILED: Detected {bad_permissions} permission violations!")
        return False
    if total_reads < 500:
        print(f"FAILED: Not enough reads completed ({total_reads})")
        return False

    print("[PASS] Challenge 2: 400 mutations under continuous readers achieved 0 torn reads and 100% 0600 mode.")
    return True

# ---------------------------------------------------------------------------
# Test 3: Abrupt Client Disconnections & Pipelining Stress (UAF / Crash check)
# ---------------------------------------------------------------------------
def test_abrupt_disconnects_and_pipelining(sbox):
    print("=== [CHALLENGE 3] Abrupt Disconnects & Pipelining Stress (UAF check) ===")
    ctx = DaemonContext(sbox, "disconnect_stress")
    ctx.start()

    # Sub-test 3A: Pipelined flurry with abrupt RST
    for cycle in range(50):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(ctx.sock_path)
        # Send 50 pipelined status queries
        payload = b"status\n" * 50
        s.sendall(payload)
        # Read partial bytes
        try:
            s.recv(64)
        except Exception:
            pass
        # Abrupt reset using SO_LINGER 0
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\x00\x00\x00\x00\x00\x00\x00')
        s.close()

    time.sleep(0.1)
    # Check daemon is healthy and responsive
    resp = send_cmd(ctx.sock_path, "status\n")
    if not resp.startswith("{"):
        print(f"FAILED: Daemon unresponsive after SO_LINGER blasts: {resp}")
        ctx.stop_kill()
        return False

    # Sub-test 3B: Pipelined commands with shutdown(SHUT_RD)
    for cycle in range(50):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(ctx.sock_path)
        s.sendall(b"status\nstatus\nnoise anc\nambient-level 10\nstatus\n")
        try:
            s.shutdown(socket.SHUT_RD)
        except Exception:
            pass
        s.close()

    time.sleep(0.1)
    resp = send_cmd(ctx.sock_path, "status\n")
    if not resp.startswith("{"):
        print("FAILED: Daemon unresponsive after SHUT_RD blasts")
        ctx.stop_kill()
        return False

    ret = ctx.stop_term()
    if ret != 0:
        print(f"FAILED: Daemon crashed or returned code {ret}")
        return False

    print("[PASS] Challenge 3: 100 abrupt disconnect and RST cycles survived with 0 UAF / 0 crash.")
    return True

# ---------------------------------------------------------------------------
# Test 4: Clean Shutdown & Stale Recovery
# ---------------------------------------------------------------------------
def test_clean_shutdown_and_stale_recovery(sbox):
    print("=== [CHALLENGE 4] Clean Shutdown (SIGTERM / SIGINT) & Stale Recovery ===")
    ctx = DaemonContext(sbox, "shutdown_stress")

    # 4A: Normal SIGTERM shutdown
    ctx.start()
    if not os.path.exists(ctx.sock_path) or not os.path.exists(ctx.status_path):
        print("FAILED: Initial socket or status.json missing")
        ctx.stop_kill()
        return False

    ret = ctx.stop_term()
    if ret != 0:
        print(f"FAILED: SIGTERM shutdown exited with {ret}")
        return False
    if os.path.exists(ctx.sock_path):
        print(f"FAILED: Socket {ctx.sock_path} was not unlinked on SIGTERM")
        return False
    if os.path.exists(ctx.status_path):
        print(f"FAILED: status.json {ctx.status_path} was not unlinked on SIGTERM")
        return False

    # 4B: Normal SIGINT shutdown
    ctx.start()
    ret = ctx.stop_int()
    if ret != 0:
        print(f"FAILED: SIGINT shutdown exited with {ret}")
        return False
    if os.path.exists(ctx.sock_path):
        print(f"FAILED: Socket {ctx.sock_path} was not unlinked on SIGINT")
        return False
    if os.path.exists(ctx.status_path):
        print(f"FAILED: status.json {ctx.status_path} was not unlinked on SIGINT")
        return False

    # 4C: Stale recovery after SIGKILL
    ctx.start()
    ctx.stop_kill() # abrupt kill leaves socket and status.json intact
    if not os.path.exists(ctx.sock_path):
        print("FAILED: Expected stale socket to exist after SIGKILL")
        return False

    # Create stale temporary file to test stale temp file resilience
    stale_tmp = ctx.status_path + ".tmp.99999"
    with open(stale_tmp, "w") as f:
        f.write("stale garbage")

    # Start daemon again over stale socket and stale tmp
    ctx.start()
    # Query status
    resp = send_cmd(ctx.sock_path, "status\n")
    if not resp.startswith("{"):
        print(f"FAILED: Unable to query status after stale socket recovery: {resp}")
        ctx.stop_kill()
        return False

    ret = ctx.stop_term()
    if ret != 0:
        print(f"FAILED: Daemon failed to stop cleanly after recovery: {ret}")
        return False

    print("[PASS] Challenge 4: SIGTERM/SIGINT clean unlinking and stale recovery verified.")
    return True

# ---------------------------------------------------------------------------
# Test 5: Pipelined Mixed Commands & Sequential Ordering
# ---------------------------------------------------------------------------
def test_pipelined_mixed_commands(sbox):
    print("=== [CHALLENGE 5] Pipelined Mixed Valid/Invalid Commands & Sequential Ordering ===")
    ctx = DaemonContext(sbox, "pipelined_mixed")
    ctx.start()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(ctx.sock_path)

    # Pipeline 6 commands in a single buffer
    cmds = (
        "status\n"
        "noise anc\n"
        "ambient-level 999\n"
        "status\n"
        "boguscommand\n"
        "status\n"
    )
    s.sendall(cmds.encode("utf-8"))

    # Read exactly 6 lines
    lines = []
    buffer = ""
    start_t = time.time()
    while len(lines) < 6 and time.time() - start_t < 3.0:
        chunk = s.recv(1024).decode("utf-8")
        if not chunk:
            break
        buffer += chunk
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            lines.append(line)

    s.close()
    ctx.stop_term()

    if len(lines) != 6:
        print(f"FAILED: Expected 6 lines in response, got {len(lines)}: {lines}")
        return False

    # Verify line 0: status (compact JSON)
    if not lines[0].startswith("{") or "schema_version" not in lines[0]:
        print(f"FAILED: Line 0 is not valid JSON: {lines[0]}")
        return False

    # Verify line 1: OK (noise anc)
    if lines[1] != "OK":
        print(f"FAILED: Line 1 expected 'OK', got '{lines[1]}'")
        return False

    # Verify line 2: ERR (ambient-level 999)
    if not lines[2].startswith("ERR"):
        print(f"FAILED: Line 2 expected 'ERR', got '{lines[2]}'")
        return False

    # Verify line 3: status (compact JSON with noise_mode == anc)
    st3 = json.loads(lines[3])
    if st3.get("noise_mode") != "anc":
        print(f"FAILED: Line 3 noise_mode is {st3.get('noise_mode')}, expected anc")
        return False

    # Verify line 4: ERR (boguscommand)
    if not lines[4].startswith("ERR unknown command"):
        print(f"FAILED: Line 4 expected 'ERR unknown command', got '{lines[4]}'")
        return False

    # Verify line 5: status (compact JSON)
    st5 = json.loads(lines[5])
    if st5.get("schema_version") != 1:
        print(f"FAILED: Line 5 schema_version is invalid")
        return False

    print("[PASS] Challenge 5: Pipelined mixed valid/invalid commands kept strict sequential sync.")
    return True

# ---------------------------------------------------------------------------
# Main Runner
# ---------------------------------------------------------------------------
def main():
    sbox = tempfile.mkdtemp(prefix="omasonyxm3_challenger_")
    print(f"============================================================")
    print(f"  Milestone 2 Adversarial Challenger Harness")
    print(f"  Binary: {DAEMON_BIN}")
    print(f"  Sandbox: {sbox}")
    print(f"============================================================")

    all_passed = True
    try:
        if not test_rapid_concurrent_status(sbox):
            all_passed = False
        if not test_atomic_file_updates_under_load(sbox):
            all_passed = False
        if not test_abrupt_disconnects_and_pipelining(sbox):
            all_passed = False
        if not test_clean_shutdown_and_stale_recovery(sbox):
            all_passed = False
        if not test_pipelined_mixed_commands(sbox):
            all_passed = False
    finally:
        import shutil
        shutil.rmtree(sbox, ignore_errors=True)

    print("============================================================")
    if all_passed:
        print("  ALL CHALLENGER ADVERSARIAL TESTS PASSED (100% SUCCESS)")
        print("============================================================")
        return 0
    else:
        print("  SOME CHALLENGER TESTS FAILED!")
        print("============================================================")
        return 1

if __name__ == "__main__":
    sys.exit(main())
