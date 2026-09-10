#!/usr/bin/env python3
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

# Resolved relative to the repository so the suite is not tied to one checkout.
DAEMON_BIN = os.environ.get("DAEMON_BIN", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "daemon", "sony-xm3-daemon"))

def check_mode(path, expected_mode):
    st = os.stat(path)
    actual_mode = stat.S_IMODE(st.st_mode)
    return actual_mode == expected_mode, actual_mode

def send_ipc_cmd(sock_path, cmd, timeout=2.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall(cmd.encode("utf-8"))
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(1024)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.decode("utf-8")

# ---------------------------------------------------------------------------
# Test 1: Mock Daemon Start & Stop Cycles (30 cycles)
# ---------------------------------------------------------------------------
def test_start_stop_cycles(sbox):
    print("=== [LIFECYCLE STRESS] Test 1: 30 Start / Stop Cycles ===")
    state_dir = os.path.join(sbox, "state_cycles")
    run_dir = os.path.join(sbox, "run_cycles")
    sock_path = os.path.join(run_dir, "sony-xm3.sock")
    status_path = os.path.join(state_dir, "sony-xm3", "status.json")

    for i in range(1, 31):
        proc = subprocess.Popen(
            [DAEMON_BIN, "--mock", "--state-dir", state_dir, "--runtime-dir", run_dir],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        # Wait for ready line
        ready = False
        start_t = time.time()
        while time.time() - start_t < 3.0:
            line = proc.stdout.readline()
            if "[MOCK_DAEMON] Ready" in line:
                ready = True
                break
            time.sleep(0.01)

        if not ready:
            proc.kill()
            out, err = proc.communicate()
            print(f"FAIL: Cycle {i} daemon did not output Ready! Out: {out}, Err: {err}")
            return False

        # Verify permissions
        if not os.path.exists(sock_path):
            print(f"FAIL: Cycle {i} socket file {sock_path} does not exist")
            proc.kill()
            return False

        ok_sock, mode_sock = check_mode(sock_path, 0o700)
        if not ok_sock:
            print(f"FAIL: Cycle {i} socket mode is {oct(mode_sock)}, expected 0700")
            proc.kill()
            return False

        if not os.path.exists(status_path):
            print(f"FAIL: Cycle {i} status.json does not exist")
            proc.kill()
            return False

        ok_stat, mode_stat = check_mode(status_path, 0o600)
        if not ok_stat:
            print(f"FAIL: Cycle {i} status.json mode is {oct(mode_stat)}, expected 0600")
            proc.kill()
            return False

        # Verify IPC query
        resp = send_ipc_cmd(sock_path, "status\n")
        try:
            st = json.loads(resp)
            if st.get("schema_version") != 1 or not st.get("connected"):
                print(f"FAIL: Cycle {i} invalid status payload: {resp}")
                proc.kill()
                return False
        except Exception as e:
            print(f"FAIL: Cycle {i} JSON parse error: {e}, resp: {resp}")
            proc.kill()
            return False

        # Send SIGTERM
        proc.send_signal(signal.SIGTERM)
        ret = proc.wait(timeout=3.0)
        if ret != 0:
            print(f"FAIL: Cycle {i} daemon exited with non-zero code {ret}")
            return False

        # Verify clean unlinking
        if os.path.exists(sock_path):
            print(f"FAIL: Cycle {i} socket {sock_path} was not unlinked on SIGTERM")
            return False
        if os.path.exists(status_path):
            print(f"FAIL: Cycle {i} status.json {status_path} was not unlinked on SIGTERM")
            return False

    print("[PASS] Test 1: 30 Clean Start/Stop cycles with 0700 socket, 0600 file, and clean unlinking")
    return True

# ---------------------------------------------------------------------------
# Test 2: Rapid Signal Stress (SIGUSR1 / SIGUSR2)
# ---------------------------------------------------------------------------
def test_rapid_signals(sbox):
    print("=== [LIFECYCLE STRESS] Test 2: Rapid SIGUSR1 / SIGUSR2 Signal Blasts ===")
    state_dir = os.path.join(sbox, "state_sig")
    run_dir = os.path.join(sbox, "run_sig")
    sock_path = os.path.join(run_dir, "sony-xm3.sock")
    status_path = os.path.join(state_dir, "sony-xm3", "status.json")

    proc = subprocess.Popen(
        [DAEMON_BIN, "--mock", "--state-dir", state_dir, "--runtime-dir", run_dir],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    ready = False
    start_t = time.time()
    while time.time() - start_t < 3.0:
        line = proc.stdout.readline()
        if "[MOCK_DAEMON] Ready" in line:
            ready = True
            break
        time.sleep(0.01)

    if not ready:
        proc.kill()
        return False

    stop_readers = False
    torn_reads = 0
    total_reads = 0
    ipc_errors = 0
    total_ipc = 0

    def file_reader():
        nonlocal torn_reads, total_reads, stop_readers
        while not stop_readers:
            try:
                if os.path.exists(status_path):
                    with open(status_path, "r") as f:
                        txt = f.read()
                    if txt:
                        total_reads += 1
                        data = json.loads(txt)
                        if "connected" not in data or "schema_version" not in data:
                            torn_reads += 1
            except Exception:
                torn_reads += 1
            time.sleep(0.001)

    def ipc_reader():
        nonlocal ipc_errors, total_ipc, stop_readers
        while not stop_readers:
            try:
                resp = send_ipc_cmd(sock_path, "status\n", timeout=1.0)
                total_ipc += 1
                data = json.loads(resp)
                if "connected" not in data:
                    ipc_errors += 1
            except Exception:
                ipc_errors += 1
            time.sleep(0.005)

    reader_thread = threading.Thread(target=file_reader)
    ipc_thread = threading.Thread(target=ipc_reader)
    reader_thread.start()
    ipc_thread.start()

    # Blast 100 alternating SIGUSR1 and SIGUSR2
    for i in range(100):
        sig = signal.SIGUSR1 if (i % 2 == 0) else signal.SIGUSR2
        try:
            os.kill(proc.pid, sig)
        except ProcessLookupError:
            print("FAIL: Daemon crashed during signal blasting!")
            stop_readers = True
            reader_thread.join()
            ipc_thread.join()
            return False
        time.sleep(0.008)

    time.sleep(0.1)
    stop_readers = True
    reader_thread.join()
    ipc_thread.join()

    # Check daemon is still alive and responsive
    resp = send_ipc_cmd(sock_path, "status\n")
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=3.0)

    print(f"[LIFECYCLE STRESS] Signal test results: Total file reads: {total_reads}, Torn: {torn_reads}, Total IPC: {total_ipc}, IPC errs: {ipc_errors}")
    if torn_reads > 0:
        print(f"FAIL: Detected {torn_reads} torn reads during signal blast")
        return False
    if ipc_errors > 0:
        print(f"FAIL: Detected {ipc_errors} IPC errors during signal blast")
        return False

    print("[PASS] Test 2: Daemon handled 100 rapid SIGUSR1/SIGUSR2 signals with 0 torn reads and 0 IPC errors")
    return True

# ---------------------------------------------------------------------------
# Test 3: High-Concurrency IPC Client Stress (20 clients, 1,000 commands)
# ---------------------------------------------------------------------------
def test_high_concurrency_ipc(sbox):
    print("=== [LIFECYCLE STRESS] Test 3: 20 Concurrent IPC Clients (1,000 commands total) ===")
    state_dir = os.path.join(sbox, "state_ipc")
    run_dir = os.path.join(sbox, "run_ipc")
    sock_path = os.path.join(run_dir, "sony-xm3.sock")

    proc = subprocess.Popen(
        [DAEMON_BIN, "--mock", "--state-dir", state_dir, "--runtime-dir", run_dir],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    ready = False
    start_t = time.time()
    while time.time() - start_t < 3.0:
        line = proc.stdout.readline()
        if "[MOCK_DAEMON] Ready" in line:
            ready = True
            break
        time.sleep(0.01)

    if not ready:
        proc.kill()
        return False

    num_clients = 20
    cmds_per_client = 50
    client_errors = []

    def client_worker(cid):
        commands = [
            ("noise anc\n", "OK\n"),
            ("ambient-level 14\n", "OK\n"),
            ("eq vocal\n", "OK\n"),
            ("dsee on\n", "OK\n"),
            ("voice-focus off\n", "OK\n"),
            ("surround arena\n", "OK\n"),
            ("ear-detect on\n", "OK\n"),
            ("eq custom 1 2 3 4 5 2\n", "OK\n"),
            ("status\n", "JSON"),
        ]
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(3.0)
            s.connect(sock_path)

            for i in range(cmds_per_client):
                cmd, exp = commands[i % len(commands)]
                s.sendall(cmd.encode("utf-8"))
                resp = b""
                while not resp.endswith(b"\n"):
                    chunk = s.recv(1024)
                    if not chunk:
                        break
                    resp += chunk
                resp_str = resp.decode("utf-8")
                if exp == "OK\n":
                    if resp_str != "OK\n":
                        client_errors.append(f"Client {cid} cmd {cmd.strip()} expected OK\\n got {resp_str}")
                elif exp == "JSON":
                    try:
                        d = json.loads(resp_str)
                        if "schema_version" not in d:
                            client_errors.append(f"Client {cid} status JSON missing schema_version")
                    except Exception as e:
                        client_errors.append(f"Client {cid} status JSON decode error {e}: {resp_str}")
            s.close()
        except Exception as e:
            client_errors.append(f"Client {cid} exception: {e}")

    threads = []
    for c in range(num_clients):
        t = threading.Thread(target=client_worker, args=(c,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=3.0)

    if client_errors:
        print(f"FAIL: {len(client_errors)} client errors occurred! Sample:")
        for err in client_errors[:10]:
            print("  ", err)
        return False

    print(f"[PASS] Test 3: 20 Concurrent clients completed {num_clients * cmds_per_client} commands with 0 errors")
    return True

# ---------------------------------------------------------------------------
# Test 4: Stale Socket Recovery After SIGKILL
# ---------------------------------------------------------------------------
def test_stale_socket_recovery(sbox):
    print("=== [LIFECYCLE STRESS] Test 4: Stale Socket Recovery After SIGKILL ===")
    state_dir = os.path.join(sbox, "state_kill")
    run_dir = os.path.join(sbox, "run_kill")
    sock_path = os.path.join(run_dir, "sony-xm3.sock")

    # 1. Start daemon
    proc = subprocess.Popen(
        [DAEMON_BIN, "--mock", "--state-dir", state_dir, "--runtime-dir", run_dir],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    ready = False
    start_t = time.time()
    while time.time() - start_t < 3.0:
        line = proc.stdout.readline()
        if "[MOCK_DAEMON] Ready" in line:
            ready = True
            break
        time.sleep(0.01)

    if not ready:
        proc.kill()
        return False

    if not os.path.exists(sock_path):
        print("FAIL: Socket file was not created")
        proc.kill()
        return False

    # 2. Kill ungracefully with SIGKILL
    proc.send_signal(signal.SIGKILL)
    proc.wait()

    # Verify socket is still on disk (stale socket)
    if not os.path.exists(sock_path):
        print("FAIL: Expected socket to remain after SIGKILL")
        return False

    # 3. Start daemon again on same path without manual cleanup
    proc2 = subprocess.Popen(
        [DAEMON_BIN, "--mock", "--state-dir", state_dir, "--runtime-dir", run_dir],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    ready2 = False
    start_t = time.time()
    while time.time() - start_t < 3.0:
        line = proc2.stdout.readline()
        if "[MOCK_DAEMON] Ready" in line:
            ready2 = True
            break
        time.sleep(0.01)

    if not ready2:
        proc2.kill()
        out, err = proc2.communicate()
        print(f"FAIL: Second daemon failed to bind over stale socket! Out: {out}, Err: {err}")
        return False

    # Verify client can connect immediately
    resp = send_ipc_cmd(sock_path, "noise wind\n")
    if resp != "OK\n":
        print(f"FAIL: Command over recovered socket returned '{resp}'")
        proc2.kill()
        return False

    proc2.send_signal(signal.SIGTERM)
    proc2.wait(timeout=3.0)

    if os.path.exists(sock_path):
        print("FAIL: Socket was not cleanly unlinked on SIGTERM")
        return False

    print("[PASS] Test 4: Daemon successfully recovered from stale socket after SIGKILL")
    return True

def main():
    sbox = tempfile.mkdtemp(prefix="omasonyxm3_daemon_stress_")
    print(f"Lifecycle stress sandbox: {sbox}")
    all_ok = True

    try:
        all_ok = test_start_stop_cycles(sbox) and all_ok
        all_ok = test_rapid_signals(sbox) and all_ok
        all_ok = test_high_concurrency_ipc(sbox) and all_ok
        all_ok = test_stale_socket_recovery(sbox) and all_ok
    finally:
        import shutil
        shutil.rmtree(sbox, ignore_errors=True)

    if all_ok:
        print("\n========================================")
        print("  ALL DAEMON LIFECYCLE STRESS TESTS PASSED")
        print("========================================")
        sys.exit(0)
    else:
        print("\n========================================")
        print("  SOME DAEMON LIFECYCLE STRESS TESTS FAILED")
        print("========================================")
        sys.exit(1)

if __name__ == "__main__":
    main()
