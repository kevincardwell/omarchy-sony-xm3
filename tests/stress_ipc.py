#!/usr/bin/env python3
"""
Empirical Stress Test Suite for IPC Server & Wire Protocol
Omarchy-Sony Project - Milestone 2 Adversarial Review
"""

import os
import sys
import time
import socket
import struct
import signal
import subprocess
import tempfile
import json
from pathlib import Path

# Color helpers
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
BLUE = "\033[94m"
RESET = "\033[0m"

class TestFailure(Exception):
    pass

class DaemonHarness:
    def __init__(self):
        self.tmp_dir = tempfile.TemporaryDirectory(prefix="omasonyxm3_ipc_stress_")
        self.state_dir = Path(self.tmp_dir.name) / "state"
        self.runtime_dir = Path(self.tmp_dir.name) / "run"
        self.state_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.runtime_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.socket_path = self.runtime_dir / "sony-xm3.sock"
        self.daemon_proc = None
        bin_env = os.environ.get("DAEMON_BIN")
        if bin_env:
            self.daemon_bin = Path(bin_env)
        else:
            self.daemon_bin = Path(__file__).resolve().parent.parent / "build" / "daemon" / "sony-xm3-daemon"

    def start(self):
        if not self.daemon_bin.exists():
            raise RuntimeError(f"Daemon binary not found at {self.daemon_bin}")
        
        cmd = [
            str(self.daemon_bin),
            "--mock",
            "--state-dir", str(self.state_dir),
            "--runtime-dir", str(self.runtime_dir)
        ]
        
        self.daemon_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        # Wait up to 5s for socket to become ready
        start_t = time.time()
        while time.time() - start_t < 5.0:
            if self.daemon_proc.poll() is not None:
                out, err = self.daemon_proc.communicate()
                raise RuntimeError(f"Daemon exited unexpectedly with code {self.daemon_proc.returncode}:\n{out}\n{err}")
            if self.socket_path.exists():
                # Verify we can connect
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(str(self.socket_path))
                    s.close()
                    return
                except Exception:
                    pass
            time.sleep(0.05)
        raise TimeoutError("Timed out waiting for daemon socket to initialize")

    def is_alive(self):
        return self.daemon_proc is not None and self.daemon_proc.poll() is None

    def get_fd_count(self):
        if not self.is_alive():
            return -1
        fd_path = f"/proc/{self.daemon_proc.pid}/fd"
        try:
            return len(os.listdir(fd_path))
        except Exception as e:
            return -1

    def connect(self, timeout=2.0):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(str(self.socket_path))
        return s

    def stop(self):
        if self.daemon_proc and self.daemon_proc.poll() is None:
            self.daemon_proc.send_signal(signal.SIGTERM)
            try:
                self.daemon_proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.daemon_proc.kill()
                self.daemon_proc.wait()
        self.tmp_dir.cleanup()


def run_tests():
    print(f"{BLUE}======================================================{RESET}")
    print(f"{BLUE}   Milestone 2 IPC Server Adversarial Stress Suite    {RESET}")
    print(f"{BLUE}======================================================{RESET}")

    harness = DaemonHarness()
    harness.start()
    pid = harness.daemon_proc.pid
    baseline_fds = harness.get_fd_count()
    print(f"[*] Started mock daemon (PID: {pid}, baseline FDs: {baseline_fds})")

    passed = 0
    failed = 0

    def record_test(name, fn):
        nonlocal passed, failed
        print(f"[*] Running: {name} ... ", end="", flush=True)
        try:
            if not harness.is_alive():
                raise TestFailure("Daemon died before test started!")
            fn()
            if not harness.is_alive() and "shutdown" not in name.lower():
                out, err = harness.daemon_proc.communicate()
                raise TestFailure(f"Daemon crashed during test! Output:\n{out}\nError:\n{err}")
            print(f"{GREEN}[PASS]{RESET}")
            passed += 1
        except Exception as e:
            if not harness.is_alive():
                out, err = harness.daemon_proc.communicate()
                print(f"{RED}[FAIL]{RESET} -> {e} (Daemon crashed! code={harness.daemon_proc.returncode}, err={err.strip()}, out={out.strip()})")
            else:
                print(f"{RED}[FAIL]{RESET} -> {e}")
            failed += 1

    # -------------------------------------------------------------
    # 1. Malformed commands: Newlines, whitespace, empty commands
    # -------------------------------------------------------------
    def test_empty_and_whitespace():
        with harness.connect() as s:
            # Empty command
            s.sendall(b"\n")
            res = s.recv(1024)
            if not res.startswith(b"ERR"):
                raise TestFailure(f"Expected ERR on empty line, got {res}")
            
            # Whitespace commands
            for payload in [b"   \n", b"\t\t\n", b"  \r\n", b"\r\n"]:
                s.sendall(payload)
                res = s.recv(1024)
                if not res.startswith(b"ERR"):
                    raise TestFailure(f"Expected ERR on whitespace, got {res}")

    record_test("1. Empty and whitespace commands", test_empty_and_whitespace)

    # -------------------------------------------------------------
    # 2. Unknown verbs and invalid arguments
    # -------------------------------------------------------------
    def test_invalid_syntax():
        with harness.connect() as s:
            bad_cmds = [
                b"foobar\n",
                b"noise\n",
                b"noise superanc\n",
                b"ambient-level\n",
                b"ambient-level -1\n",
                b"ambient-level 20\n",
                b"ambient-level notanumber\n",
                b"eq\n",
                b"eq nonexistent\n",
                b"eq custom 1 2 3\n",
                b"eq custom 1 2 3 4 5 11\n",
                b"eq custom 1 2 3 4 5 -11\n",
                b"eq custom 1 2 3 4 11 0\n",
                b"voice-focus\n",
                b"voice-focus maybe\n",
                b"dsee\n",
                b"dsee fast\n",
                b"surround\n",
                b"surround stadium\n",
                b"dsee\n",
                b"dsee true_or_false\n",
            ]
            for cmd in bad_cmds:
                s.sendall(cmd)
                res = s.recv(1024)
                if not res.startswith(b"ERR"):
                    raise TestFailure(f"Command {cmd} should have returned ERR, got {res}")

    record_test("2. Invalid verbs & argument boundary rejection", test_invalid_syntax)

    # -------------------------------------------------------------
    # 3. Binary Garbage Injection
    # -------------------------------------------------------------
    def test_binary_garbage():
        with harness.connect() as s:
            # Null bytes before command
            s.sendall(b"\x00\x00noise anc\n")
            res = s.recv(1024)
            # Should safely reject as ERR or process cleanly without crash
            if not (res.startswith(b"ERR") or res == b"OK\n"):
                raise TestFailure(f"Unexpected response to null byte injection: {res}")
            
            # High ASCII / Unicode garbage
            s.sendall(b"\xff\xfe\xef\xaa\xbb\xcc\xdd\n")
            res = s.recv(1024)
            if not res.startswith(b"ERR"):
                raise TestFailure(f"Expected ERR for high ascii garbage, got {res}")

            # Random binary garbage chunks
            garbage = os.urandom(256).replace(b"\n", b"") + b"\n"
            s.sendall(garbage)
            res = s.recv(1024)
            if not res.startswith(b"ERR"):
                raise TestFailure(f"Expected ERR for random binary chunk, got {res}")

    record_test("3. Binary garbage injection", test_binary_garbage)

    # -------------------------------------------------------------
    # 4. Oversized lines (>64KB)
    # -------------------------------------------------------------
    def test_oversized_lines():
        # Server maxLineLength is 4096. Test sending > 64KB (e.g. 70KB, 128KB)
        for size in [5000, 65536, 70000, 131072]:
            with harness.connect() as s:
                junk = b"A" * size
                try:
                    s.sendall(junk)
                    # Now try to read; server should return "ERR line too long\n" and close socket
                    res = s.recv(1024)
                    if b"ERR line too long" not in res:
                        raise TestFailure(f"Expected 'ERR line too long' for size {size}, got: {res}")
                    # Next read should be EOF (connection closed by server)
                    eof = s.recv(1024)
                    if len(eof) != 0:
                        raise TestFailure(f"Expected EOF after line length exceeded, got {len(eof)} bytes")
                except (BrokenPipeError, ConnectionResetError):
                    # Closing immediately or RST is also acceptable
                    pass

    record_test("4. Oversized lines (>64KB) enforcement", test_oversized_lines)

    # -------------------------------------------------------------
    # 5. Continuous Stream Flood (1MB)
    # -------------------------------------------------------------
    def test_1mb_flood():
        with harness.connect() as s:
            # Send continuous stream of 1MB in 16KB blocks
            sent = 0
            block = b"X" * 16384
            closed = False
            for _ in range(64):
                try:
                    s.sendall(block)
                    sent += len(block)
                except (BrokenPipeError, ConnectionResetError):
                    closed = True
                    break
            # Verify server closed connection
            if not closed:
                try:
                    res = s.recv(1024)
                    if b"ERR line too long" not in res:
                        raise TestFailure(f"Expected ERR line too long on 1MB flood, got {res}")
                except (BrokenPipeError, ConnectionResetError):
                    pass

    record_test("5. 1MB un-delimited byte stream flood", test_1mb_flood)

    # -------------------------------------------------------------
    # 6. Pipelined commands in single buffer
    # -------------------------------------------------------------
    def test_pipelined_commands():
        with harness.connect() as s:
            # Send 10 commands in a single buffer
            cmds = [
                "status\n",
                "noise anc\n",
                "ambient-level 12\n",
                "eq vocal\n",
                "voice-focus on\n",
                "dsee off\n",
                "surround arena\n",
                "dsee off\n",
                "status\n",
                "noise off\n"
            ]
            s.sendall("".join(cmds).encode("utf-8"))
            
            # Read all responses
            full_resp = b""
            while len(full_resp.splitlines()) < len(cmds):
                chunk = s.recv(4096)
                if not chunk:
                    break
                full_resp += chunk
            
            lines = full_resp.splitlines()
            if len(lines) != len(cmds):
                raise TestFailure(f"Expected {len(cmds)} responses, got {len(lines)}: {lines}")
            
            # Verify first status is JSON
            json1 = json.loads(lines[0])
            if json1.get("schema_version") != 1:
                raise TestFailure(f"Invalid JSON in pipelined status response: {lines[0]}")
            
            # Verify intermediate OKs
            for i in range(1, 8):
                if lines[i] != b"OK":
                    raise TestFailure(f"Expected OK at line {i}, got {lines[i]}")
            
            # Verify second status has updated values. `ambient-level 12` lands
            # the headset in ambient mode: on the XM3 the level and the mode are
            # the same axis, so the later command wins over `noise anc`.
            json2 = json.loads(lines[8])
            if json2.get("noise_mode") != "ambient" or json2.get("ambient_level") != 12:
                raise TestFailure(f"Pipelined state updates not reflected in second status: {lines[8]}")
            if json2.get("eq_preset") != "vocal" or json2.get("voice_passthrough") is not True:
                raise TestFailure(f"Pipelined EQ / voice focus not reflected: {lines[8]}")
            if json2.get("surround") != "arena" or json2.get("dsee_hx") is not False:
                raise TestFailure(f"Pipelined surround / DSEE not reflected: {lines[8]}")
            
            if lines[9] != b"OK":
                raise TestFailure(f"Expected OK at line 9, got {lines[9]}")

    record_test("6. Pipelined commands in single buffer (10 commands)", test_pipelined_commands)

    # -------------------------------------------------------------
    # 7. Large Pipelined Burst (100 commands in single write)
    # -------------------------------------------------------------
    def test_large_pipelined_burst():
        with harness.connect() as s:
            batch_count = 100
            batch = ("ambient-level 5\nstatus\n" * 50).encode("utf-8")
            s.sendall(batch)
            
            resp = b""
            while len(resp.splitlines()) < batch_count:
                chunk = s.recv(8192)
                if not chunk:
                    break
                resp += chunk
            
            lines = resp.splitlines()
            if len(lines) != batch_count:
                raise TestFailure(f"Expected {batch_count} lines in response, got {len(lines)}")

    record_test("7. Large pipelined burst (100 commands)", test_large_pipelined_burst)

    # -------------------------------------------------------------
    # 8. Partial writes & Byte-at-a-time streaming
    # -------------------------------------------------------------
    def test_byte_at_a_time_streaming():
        with harness.connect() as s:
            cmd = b"status\n"
            for b in cmd:
                s.sendall(bytes([b]))
                time.sleep(0.01) # 10ms between bytes
            
            resp = b""
            while not resp.endswith(b"\n"):
                chunk = s.recv(1024)
                if not chunk:
                    break
                resp += chunk
            
            parsed = json.loads(resp.decode("utf-8").strip())
            if parsed.get("schema_version") != 1:
                raise TestFailure(f"Unexpected status response: {resp}")

    record_test("8. Partial writes & Byte-at-a-time streaming (10ms delay)", test_byte_at_a_time_streaming)

    # -------------------------------------------------------------
    # 9. Rapid Connect / Disconnect Bursts (500+ cycles)
    # -------------------------------------------------------------
    def test_rapid_connect_disconnect_cycles():
        CYCLES = 500
        start_t = time.time()
        for i in range(CYCLES):
            s = harness.connect()
            # Immediately close without sending data
            s.close()
            if i % 100 == 0:
                # Ensure daemon is still responsive
                with harness.connect() as probe:
                    probe.sendall(b"status\n")
                    res = probe.recv(1024)
                    if b"schema_version" not in res:
                        raise TestFailure(f"Daemon unresponsive at cycle {i}")
        elapsed = time.time() - start_t
        print(f" ({CYCLES} cycles in {elapsed:.2f}s) ", end="")

    record_test("9. Rapid connect/disconnect bursts (500 cycles)", test_rapid_connect_disconnect_cycles)

    # -------------------------------------------------------------
    # 10. Rapid Connect, Partial Write, Disconnect Bursts (200 cycles)
    # -------------------------------------------------------------
    def test_rapid_partial_write_disconnect():
        CYCLES = 200
        for i in range(CYCLES):
            s = harness.connect()
            # Send incomplete command prefix
            s.sendall(b"ambient-")
            s.close()
        
        # Verify daemon is healthy
        with harness.connect() as probe:
            probe.sendall(b"noise anc\n")
            res = probe.recv(1024)
            if res != b"OK\n":
                raise TestFailure(f"Daemon did not handle command after partial write bursts: {res}")

    record_test("10. Rapid connect, partial write, disconnect bursts (200 cycles)", test_rapid_partial_write_disconnect)

    # -------------------------------------------------------------
    # 11. Abrupt TCP/UNIX RST (SO_LINGER 0)
    # -------------------------------------------------------------
    def test_abrupt_rst():
        for _ in range(100):
            s = harness.connect()
            # Configure SO_LINGER to 0 (hard abort / RST upon close)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            s.sendall(b"status\n")
            s.close()
        
        time.sleep(0.1)
        with harness.connect() as probe:
            probe.sendall(b"status\n")
            res = probe.recv(1024)
            if b"schema_version" not in res:
                raise TestFailure(f"Daemon failed after RST barrage: {res}")

    record_test("11. Abrupt socket reset (SO_LINGER 0 barrage, 100 cycles)", test_abrupt_rst)

    # -------------------------------------------------------------
    # 12. Pipelining with Abrupt Close (Targeting UAF / dangling reference)
    # -------------------------------------------------------------
    def test_pipelined_abrupt_close():
        # Target the specific code path in handleClientRead:
        # Client sends multiple commands and closes/shuts down write mid-processing
        for _ in range(100):
            s = harness.connect()
            # Send multiple commands
            payload = b"status\nnoise anc\nstatus\nambient-level 10\nstatus\n" * 5
            try:
                s.sendall(payload)
                # Immediately shut down reading to cause write errors on server
                s.shutdown(socket.SHUT_RD)
                s.close()
            except Exception:
                pass
        
        # Give event loop time to process events
        time.sleep(0.2)
        if not harness.is_alive():
            raise TestFailure("Daemon crashed during pipelined abrupt close test!")
        
        with harness.connect() as probe:
            probe.sendall(b"status\n")
            res = probe.recv(1024)
            if b"schema_version" not in res:
                raise TestFailure("Daemon failed after pipelined abrupt close")

    record_test("12. Pipelined commands with abrupt client shutdown (UAF check)", test_pipelined_abrupt_close)

    # -------------------------------------------------------------
    # 13. File Descriptor Leak Verification
    # -------------------------------------------------------------
    def test_fd_leak():
        time.sleep(0.3)
        final_fds = harness.get_fd_count()
        print(f" [FDs: baseline={baseline_fds}, final={final_fds}] ", end="")
        # Allow at most 1 temporary fd difference (e.g. dir listing), but must not leak hundreds
        if final_fds > baseline_fds + 2:
            raise TestFailure(f"Socket descriptor leak detected! Started with {baseline_fds}, ended with {final_fds}")

    record_test("13. File descriptor leak check", test_fd_leak)

    # -------------------------------------------------------------
    # 14. Graceful Shutdown
    # -------------------------------------------------------------
    def test_graceful_shutdown():
        harness.daemon_proc.send_signal(signal.SIGTERM)
        try:
            harness.daemon_proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            harness.daemon_proc.kill()
            raise TestFailure("Daemon failed to shut down within 3s on SIGTERM")
        
        if harness.socket_path.exists():
            raise TestFailure(f"Socket file was not unlinked on shutdown: {harness.socket_path}")

    record_test("14. Graceful shutdown on SIGTERM", test_graceful_shutdown)

    harness.stop()

    print(f"\n{BLUE}======================================================{RESET}")
    print(f"Stress Suite Summary: {passed}/{passed + failed} passed ({failed} failed)")
    print(f"{BLUE}======================================================{RESET}")

    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(run_tests())
