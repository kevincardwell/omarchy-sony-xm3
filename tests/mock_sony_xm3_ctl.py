#!/usr/bin/env python3
"""
tests/mock_sony_xm3_ctl.py — Mock / Reference Implementation of sony-xm3-ctl CLI
Conforms to PROJECT.md § sony-xm3-ctl CLI specification:
Subcommands:
    status
    noise <anc|ambient|wind|off>
    ambient-level <0-19>
    eq <preset>
    eq custom <b1> <b2> <b3> <b4> <b5> <cb>
    voice-focus <on|off>
    dsee <on|off>
    surround <off|outdoor|arena|concert|club>
    sound-position <off|front-left|front-right|front|rear-left|rear-right>
    auto-power-off <off|5min|30min|60min|180min>
    connection <quality|stable>

Exit codes:
    0: Success
    1: Error (invalid arguments, command rejected, protocol error)
    2: Daemon offline (socket does not exist or connection refused)
"""

import os
import sys
import socket
import argparse

ENUM_SUBCOMMANDS = {
    "surround": ["off", "outdoor", "arena", "concert", "club"],
    "sound-position": ["off", "front-left", "front-right", "front", "rear-left", "rear-right"],
    "auto-power-off": ["off", "5min", "30min", "60min", "180min"],
    "connection": ["quality", "stable"],
    "optimizer": ["start", "cancel"],
    "playback": ["play", "pause", "next", "previous"],
    "nc-button": ["ambient", "google-assistant", "alexa"],
    "touch-panel": ["on", "off"],
    "voice-guidance": ["on", "off"],
}


def get_default_socket_path():
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", f"/tmp/run-{os.getuid()}")
    return os.path.join(runtime_dir, "sony-xm3.sock")

def send_command(socket_path, command_str):
    if not os.path.exists(socket_path):
        sys.stderr.write(f"Error: Daemon socket does not exist at {socket_path}\n")
        return 2, ""

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(1.5)
    try:
        sock.connect(socket_path)
    except (ConnectionRefusedError, FileNotFoundError, OSError) as e:
        sys.stderr.write(f"Error: Cannot connect to daemon socket ({e})\n")
        sock.close()
        return 2, ""

    try:
        if not command_str.endswith("\n"):
            command_str += "\n"
        sock.sendall(command_str.encode("utf-8"))
        
        response = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
            if b"\n" in response:
                break
        sock.close()
        return 0, response.decode("utf-8", errors="replace").strip()
    except Exception as e:
        sys.stderr.write(f"Error: Socket communication failed ({e})\n")
        sock.close()
        return 1, ""

def main():
    parser = argparse.ArgumentParser(description="sony-xm3-ctl CLI", add_help=False)
    parser.add_argument("-s", "--socket", default=None, help="Override socket path")
    parser.add_argument("-h", "--help", action="store_true", help="Show help")
    
    args, remaining = parser.parse_known_args()

    if args.help or not remaining:
        print("Usage: sony-xm3-ctl [-s <socket>] <subcommand> [args...]")
        print("Subcommands: status, noise, ambient-level, eq, voice-focus, dsee,")
        print("             surround, sound-position, auto-power-off, connection")
        sys.exit(0 if args.help else 1)

    socket_path = args.socket or get_default_socket_path()
    subcmd = remaining[0].lower()

    if subcmd == "status":
        code, resp = send_command(socket_path, "status")
        if code != 0:
            sys.exit(code)
        print(resp)
        sys.exit(0)

    elif subcmd == "noise":
        if len(remaining) < 2:
            sys.stderr.write("Error: 'noise' requires a mode: anc, ambient, wind, off\n")
            sys.exit(1)
        mode = remaining[1].lower()
        if mode not in ["anc", "ambient", "wind", "off"]:
            sys.stderr.write(f"Error: Invalid noise mode '{mode}'\n")
            sys.exit(1)
        code, resp = send_command(socket_path, f"noise {mode}")
        if code != 0:
            sys.exit(code)
        if resp.startswith("OK"):
            print("OK")
            sys.exit(0)
        else:
            sys.stderr.write(f"Error from daemon: {resp}\n")
            sys.exit(1)

    elif subcmd == "ambient-level":
        if len(remaining) < 2:
            sys.stderr.write("Error: 'ambient-level' requires an integer level between 0 and 20\n")
            sys.exit(1)
        try:
            level = int(remaining[1])
            if not (0 <= level <= 19):
                sys.stderr.write(f"Error: Level {level} out of range [0, 19]\n")
                sys.exit(1)
        except ValueError:
            sys.stderr.write(f"Error: Invalid integer '{remaining[1]}'\n")
            sys.exit(1)
        code, resp = send_command(socket_path, f"ambient-level {level}")
        if code != 0:
            sys.exit(code)
        if resp.startswith("OK"):
            print("OK")
            sys.exit(0)
        else:
            sys.stderr.write(f"Error from daemon: {resp}\n")
            sys.exit(1)

    elif subcmd == "eq":
        if len(remaining) < 2:
            sys.stderr.write("Error: 'eq' requires a preset or 'custom' with 6 parameters\n")
            sys.exit(1)
        slot = remaining[1].lower()
        if slot in ("custom", "manual", "user1", "user2") and len(remaining) > 2:
            if len(remaining) < 8:
                sys.stderr.write("Error: 'eq custom' requires 5 bands and clear bass (6 integers between -10 and 10)\n")
                sys.exit(1)
            try:
                bands = [int(x) for x in remaining[2:7]]
                cb = int(remaining[7])
                for b in bands:
                    if not (-10 <= b <= 10):
                        sys.stderr.write("Error: Band values must be between -10 and 10\n")
                        sys.exit(1)
                if not (-10 <= cb <= 10):
                    sys.stderr.write("Error: Clear Bass must be between -10 and 10\n")
                    sys.exit(1)
            except ValueError:
                sys.stderr.write("Error: EQ parameters must be valid integers\n")
                sys.exit(1)
            cmd = f"eq {slot} " + " ".join(str(x) for x in bands) + f" {cb}"
        else:
            preset = remaining[1].lower()
            valid_presets = ["off", "bright", "excited", "mellow", "relaxed", "vocal", "treble", "bass", "speech",
                             "custom", "manual", "user1", "user2"]
            if preset not in valid_presets:
                sys.stderr.write(f"Error: Unknown EQ preset '{preset}'\n")
                sys.exit(1)
            cmd = f"eq {preset}"
        code, resp = send_command(socket_path, cmd)
        if code != 0:
            sys.exit(code)
        if resp.startswith("OK"):
            print("OK")
            sys.exit(0)
        else:
            sys.stderr.write(f"Error from daemon: {resp}\n")
            sys.exit(1)

    elif subcmd in ["voice-focus", "dsee"]:
        if len(remaining) < 2 or remaining[1].lower() not in ["on", "off"]:
            sys.stderr.write(f"Error: '{subcmd}' requires 'on' or 'off'\n")
            sys.exit(1)
        val = remaining[1].lower()
        code, resp = send_command(socket_path, f"{subcmd} {val}")
        if code != 0:
            sys.exit(code)
        if resp.startswith("OK"):
            print("OK")
            sys.exit(0)
        else:
            sys.stderr.write(f"Error from daemon: {resp}\n")
            sys.exit(1)

    elif subcmd == "volume":
        try:
            level = int(remaining[1]) if len(remaining) >= 2 else -1
        except ValueError:
            level = -1
        if not (0 <= level <= 30):
            sys.stderr.write("Error: 'volume' requires an integer between 0 and 30\n")
            sys.exit(1)
        code, resp = send_command(socket_path, f"volume {level}")
        if code != 0:
            sys.exit(code)
        if resp.startswith("OK"):
            print("OK")
            sys.exit(0)
        sys.stderr.write(f"Error from daemon: {resp}\n")
        sys.exit(1)

    elif subcmd in ENUM_SUBCOMMANDS:
        allowed = ENUM_SUBCOMMANDS[subcmd]
        if len(remaining) < 2 or remaining[1].lower() not in allowed:
            sys.stderr.write(f"Error: '{subcmd}' requires one of: {', '.join(allowed)}\n")
            sys.exit(1)
        code, resp = send_command(socket_path, f"{subcmd} {remaining[1].lower()}")
        if code != 0:
            sys.exit(code)
        if resp.startswith("OK"):
            print("OK")
            sys.exit(0)
        else:
            sys.stderr.write(f"Error from daemon: {resp}\n")
            sys.exit(1)

    else:
        sys.stderr.write(f"Error: Unknown subcommand '{subcmd}'\n")
        sys.exit(1)

if __name__ == "__main__":
    main()
