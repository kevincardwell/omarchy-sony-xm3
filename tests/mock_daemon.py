#!/usr/bin/env python3
"""
tests/mock_daemon.py — Mock Headless Daemon for Offline E2E Testing
Simulates sony-xm3-daemon:
- Listens on UNIX domain socket ($XDG_RUNTIME_DIR/sony-xm3.sock)
- Manages headphone state
- Atomically writes state updates to $XDG_STATE_HOME/sony-xm3/status.json
- Handles wire protocol commands:
    status, noise, ambient-level, eq, voice-focus, dsee,
    surround, sound-position, auto-power-off, connection
- Signal handling:
    SIGTERM / SIGINT: clean shutdown (unlinks socket and status.json)
    SIGUSR1: simulate disconnect (connected: false)
    SIGUSR2: simulate reconnect (connected: true)
"""

import os
import sys
import json
import time
import socket
import select
import signal
import argparse

# The XM3 noise-control axis: step 0 is noise cancelling, step 1 is wind noise
# reduction, and 2..ambient_max_level are ambient sound.
STEP_ANC = 0
STEP_WIND = 1
MIN_AMBIENT_STEP = 2
DEFAULT_AMBIENT_STEP = 10


def step_to_noise_mode(step):
    if step == STEP_ANC:
        return "anc"
    if step == STEP_WIND:
        return "wind"
    return "ambient"


VALID_MODES = ["anc", "ambient", "wind", "off"]
VALID_PRESETS = ["off", "bright", "excited", "mellow", "relaxed", "vocal", "treble", "bass", "speech",
                 "custom", "manual", "user1", "user2"]
CUSTOM_SLOTS = ["custom", "manual", "user1", "user2"]

class MockDaemon:
    def __init__(self, state_dir=None, runtime_dir=None):
        self.state_dir = state_dir or os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state"))
        self.runtime_dir = runtime_dir or os.environ.get("XDG_RUNTIME_DIR", f"/tmp/run-{os.getuid()}")
        
        self.sony_state_dir = os.path.join(self.state_dir, "sony-xm3")
        self.status_file = os.path.join(self.sony_state_dir, "status.json")
        self.socket_path = os.path.join(self.runtime_dir, "sony-xm3.sock")
        
        self.running = True
        self.server_sock = None
        
        # Mirrors the daemon's lastAmbientStep: switching ANC -> Ambient should
        # return to where the user left the slider.
        self.last_ambient_step = DEFAULT_AMBIENT_STEP

        self.state = {
            "schema_version": 1,
            "connected": True,
            "device_name": "WH-1000XM3",
            "battery_level": 85,
            "battery_charging": False,
            "noise_mode": "anc",
            "ambient_sound_level": 0,
            "eq_preset": "off",
            "eq_custom_bands": [0, 0, 0, 0, 0],
            "clear_bass": 0,
            "voice_passthrough": False,
            "dsee_hx": True,
            "ambient_max_level": 19,
            "surround": "off",
            "sound_position": "off",
            "auto_power_off": "180min",
            "connection_mode": "stable",
            "firmware_version": "4.5.2",
            "model_name": "WH-1000XM3",
            "optimizer_state": "idle",
            "optimizer_pressure": "1.0",
            "volume": 17,
            "volume_max": 30,
            "nc_button": "ambient",
            "touch_panel": True,
            "voice_guidance": True,
            "voice_guidance_language": "English",
            "codec": "LDAC",
            "dsee_hx_active": False,
            "last_updated": int(time.time())
        }

    def setup_directories(self):
        os.makedirs(self.sony_state_dir, mode=0o700, exist_ok=True)
        os.makedirs(self.runtime_dir, mode=0o700, exist_ok=True)

    def write_status(self):
        self.state["last_updated"] = int(time.time())
        tmp_file = f"{self.status_file}.tmp.{os.getpid()}"
        payload = self.state if self.state.get("connected", True) else {
            "schema_version": 1,
            "connected": False
        }
        with open(tmp_file, "w") as f:
            json.dump(payload, f, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp_file, 0o600)
        os.replace(tmp_file, self.status_file)

    def dsp_blocked(self, feature):
        """The XM3 cannot run EQ or VPT while streaming LDAC; mirror the daemon."""
        if self.state.get("connection_mode") == "quality":
            return (f"ERR {feature} is unavailable on Priority on sound quality (LDAC); "
                    "use `sony-xm3-ctl connection stable` to trade LDAC for it\n")
        return None

    def handle_command(self, cmd_line):
        line = cmd_line.strip()
        if not line:
            return "ERR empty command\n"
        parts = line.split()
        verb = parts[0].lower()

        if verb == "status":
            payload = self.state if self.state.get("connected", True) else {
                "schema_version": 1,
                "connected": False
            }
            return json.dumps(payload) + "\n"

        if verb == "noise":
            if len(parts) < 2:
                return "ERR missing mode (expected anc|wind|ambient|off)\n"
            mode = parts[1].lower()
            if mode not in VALID_MODES:
                return f"ERR invalid mode \x27{mode}\x27\n"

            max_level = self.state["ambient_max_level"]
            if mode == "ambient" and len(parts) >= 3:
                try:
                    level = int(parts[2])
                except ValueError:
                    return f"ERR invalid ambient level \x27{parts[2]}\x27\n"
                if not (MIN_AMBIENT_STEP <= level <= max_level):
                    return f"ERR ambient level out of range [{MIN_AMBIENT_STEP}-{max_level}]\n"
                self.last_ambient_step = level

            self.state["noise_mode"] = mode
            # On the XM3 the mode is a position on the step axis, so the level
            # has to follow the mode exactly as the real daemon does it.
            if mode == "anc":
                self.state["ambient_sound_level"] = STEP_ANC
            elif mode == "wind":
                self.state["ambient_sound_level"] = STEP_WIND
            elif mode == "ambient":
                self.state["ambient_sound_level"] = self.last_ambient_step
            self.write_status()
            return "OK\n"

        if verb == "ambient-level":
            max_level = self.state["ambient_max_level"]
            if len(parts) < 2:
                return f"ERR missing level (0-{max_level})\n"
            try:
                level = int(parts[1])
            except ValueError:
                return f"ERR invalid level \x27{parts[1]}\x27\n"
            if not (0 <= level <= max_level):
                return f"ERR ambient level out of range [0-{max_level}]\n"
            self.state["ambient_sound_level"] = level
            self.state["noise_mode"] = step_to_noise_mode(level)
            if level >= MIN_AMBIENT_STEP:
                self.last_ambient_step = level
            self.write_status()
            return "OK\n"

        if verb == "eq":
            if len(parts) < 2:
                return "ERR missing eq preset\n"
            blocked = self.dsp_blocked("EQ")
            if blocked:
                return blocked
            preset = parts[1].lower()
            # A slot name alone selects it; with six numbers it also sets bands.
            if preset in CUSTOM_SLOTS and len(parts) > 2:
                if len(parts) < 8:
                    return "ERR custom eq requires 5 bands and clear bass (6 integers [-10, 10])\n"
                try:
                    bands = [int(p) for p in parts[2:7]]
                    cb = int(parts[7])
                    for b in bands:
                        if not (-10 <= b <= 10):
                            return "ERR custom eq band out of range [-10, 10]\n"
                    if not (-10 <= cb <= 10):
                        return "ERR clear bass out of range [-10, 10]\n"
                    self.state["eq_preset"] = "custom" if preset == "manual" else preset
                    self.state["eq_custom_bands"] = bands
                    self.state["clear_bass"] = cb
                    self.write_status()
                    return "OK\n"
                except ValueError:
                    return "ERR invalid custom eq arguments\n"
            else:
                if preset not in VALID_PRESETS:
                    return f"ERR unknown eq preset \x27{preset}\x27\n"
                self.state["eq_preset"] = "custom" if preset == "manual" else preset
                self.write_status()
                return "OK\n"

        if verb == "voice-focus":
            if len(parts) < 2 or parts[1].lower() not in ["on", "off"]:
                return "ERR expected on|off\n"
            self.state["voice_passthrough"] = parts[1].lower() == "on"
            self.write_status()
            return "OK\n"

        if verb == "dsee":
            if len(parts) < 2 or parts[1].lower() not in ["on", "off"]:
                return "ERR expected on|off\n"
            self.state["dsee_hx"] = parts[1].lower() == "on"
            self.write_status()
            return "OK\n"

        if verb == "optimizer":
            if len(parts) < 2 or parts[1].lower() not in ("start", "cancel"):
                return "ERR expected start|cancel\n"
            # The real headset reports progress over a dozen seconds; the mock
            # jumps straight to the end so tests stay fast.
            self.state["optimizer_state"] = "done" if parts[1].lower() == "start" else "idle"
            self.write_status()
            return "OK\n"

        if verb == "volume":
            max_volume = self.state["volume_max"]
            try:
                level = int(parts[1]) if len(parts) >= 2 else None
            except ValueError:
                level = None
            if level is None:
                return f"ERR expected a volume 0-{max_volume}\n"
            if not (0 <= level <= max_volume):
                return f"ERR volume out of range [0-{max_volume}]\n"
            self.state["volume"] = level
            self.write_status()
            return "OK\n"

        if verb == "playback":
            if len(parts) < 2 or parts[1].lower() not in ("play", "pause", "next", "previous"):
                return "ERR expected play|pause|next|previous\n"
            return "OK\n"

        if verb in ("touch-panel", "voice-guidance"):
            if len(parts) < 2 or parts[1].lower() not in ("on", "off"):
                return "ERR expected on|off\n"
            field = "touch_panel" if verb == "touch-panel" else "voice_guidance"
            self.state[field] = parts[1].lower() == "on"
            self.write_status()
            return "OK\n"

        # Enum-valued XM3 settings. Each rejects anything outside its vocabulary
        # so the integration suite can assert on the failure path too.
        ENUM_VERBS = {
            "surround": ("surround", ["off", "outdoor", "arena", "concert", "club"]),
            "sound-position": ("sound_position",
                               ["off", "front-left", "front-right", "front", "rear-left", "rear-right"]),
            "auto-power-off": ("auto_power_off",
                               ["off", "5min", "30min", "60min", "180min"]),
            "connection": ("connection_mode", ["quality", "stable"]),
            "nc-button": ("nc_button", ["ambient", "google-assistant", "alexa"]),
        }
        if verb in ENUM_VERBS:
            field, allowed = ENUM_VERBS[verb]
            if verb in ("surround", "sound-position"):
                blocked = self.dsp_blocked("Surround" if verb == "surround" else "Sound position")
                if blocked:
                    return blocked
            if len(parts) < 2:
                return "ERR expected " + "|".join(allowed) + "\n"
            value = parts[1].lower()
            if value not in allowed:
                return f"ERR unknown {verb} value '{parts[1]}'\n"
            self.state[field] = value
            self.write_status()
            return "OK\n"

        # Internal test helper verbs
        if verb == "_set_battery":
            if len(parts) >= 2:
                self.state["battery_level"] = int(parts[1])
            if len(parts) >= 3:
                self.state["battery_charging"] = parts[2].lower() in ["true", "1", "yes"]
            self.write_status()
            return "OK\n"

        if verb == "_disconnect":
            self.state["connected"] = False
            self.write_status()
            return "OK\n"

        if verb == "_reconnect":
            self.state["connected"] = True
            self.write_status()
            return "OK\n"

        return f"ERR unknown command \x27{verb}\x27\n"

    def run(self):
        self.setup_directories()

        if os.path.exists(self.socket_path):
            try:
                os.unlink(self.socket_path)
            except OSError:
                pass

        self.server_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server_sock.bind(self.socket_path)
        os.chmod(self.socket_path, 0o700)
        self.server_sock.listen(10)
        self.server_sock.setblocking(False)

        # Initial status write
        self.write_status()

        def sig_term_handler(_signum, _frame):
            self.running = False

        def sig_usr1_handler(_signum, _frame):
            # Disconnect simulation
            self.state["connected"] = False
            self.write_status()

        def sig_usr2_handler(_signum, _frame):
            # Reconnect simulation
            self.state["connected"] = True
            self.write_status()

        signal.signal(signal.SIGTERM, sig_term_handler)
        signal.signal(signal.SIGINT, sig_term_handler)
        signal.signal(signal.SIGUSR1, sig_usr1_handler)
        signal.signal(signal.SIGUSR2, sig_usr2_handler)

        sys.stdout.write(f"[MOCK_DAEMON] Ready PID={os.getpid()}\n")
        sys.stdout.flush()

        inputs = [self.server_sock]
        clients = {}

        while self.running:
            try:
                readable, _, _ = select.select(inputs, [], [], 0.5)
            except (select.error, InterruptedError):
                continue

            for s in readable:
                if s is self.server_sock:
                    try:
                        conn, _ = self.server_sock.accept()
                        conn.setblocking(False)
                        inputs.append(conn)
                        clients[conn] = b""
                    except OSError:
                        pass
                else:
                    try:
                        data = s.recv(1024)
                        if data:
                            clients[s] += data
                            if b"\n" in clients[s]:
                                line, _, rest = clients[s].partition(b"\n")
                                clients[s] = rest
                                cmd_str = line.decode("utf-8", errors="replace")
                                response = self.handle_command(cmd_str)
                                s.sendall(response.encode("utf-8"))
                        else:
                            inputs.remove(s)
                            if s in clients:
                                del clients[s]
                            s.close()
                    except OSError:
                        inputs.remove(s)
                        if s in clients:
                            del clients[s]
                        s.close()

        # Shutdown cleanup
        try:
            if self.server_sock:
                self.server_sock.close()
            if os.path.exists(self.socket_path):
                os.unlink(self.socket_path)
            if os.path.exists(self.status_file):
                os.unlink(self.status_file)
        except OSError:
            pass
        sys.stdout.write("[MOCK_DAEMON] Shutdown cleanly\n")
        sys.stdout.flush()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=None)
    parser.add_argument("--runtime-dir", default=None)
    args = parser.parse_args()
    daemon = MockDaemon(state_dir=args.state_dir, runtime_dir=args.runtime_dir)
    daemon.run()
