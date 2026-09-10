# omarchy-sony-xm3

An Omarchy bar-widget plugin and headless C++20 daemon for managing **Sony
WH-1000XM3** headphones on Linux.

This is a port of [andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony)
(WH-1000XM5) to the XM3's older Bluetooth command set. The architecture is the
same; the protocol layer and the feature set are not — see
[Why this is a port, not a config change](#why-this-is-a-port-not-a-config-change).

---

## Requirements

**The headphones must be paired to this machine's own Bluetooth adapter.**

Control runs over an RFCOMM socket that BlueZ opens to the headset. A USB
Bluetooth *audio transmitter* dongle (Avantree DG60, TaoTronics, and similar)
pairs with the headphones itself and shows up on Linux as a USB sound card — the
host Bluetooth stack never sees the headphones, so there is nothing for the
daemon to connect to. The XM3 also holds only one host link at a time, so it
cannot be on such a dongle and on this machine simultaneously.

If audio currently reaches your headphones through a transmitter dongle, you
will need to pair them directly to this machine to use any of this. `./setup`
checks for an adapter and a paired headset and warns you if either is missing.

---

## Features

- 🔋 **Live battery & codec** — percentage, charging state, and the negotiated codec (LDAC, aptX HD, aptX, AAC, SBC).
- 🎧 **Noise control** — Noise Cancelling, Wind Noise Reduction, Ambient Sound, and Off.
- 🔊 **Ambient sound slider** — the XM3's full passthrough range, with Focus on Voice.
- 🎛️ **Equalizer** — the nine presets plus Manual (5 bands + Clear Bass) and the two saved user slots.
- 🎚️ **DSEE HX** — the XM3's audio upscaling.
- 🎪 **Surround (VPT)** — Outdoor Festival, Arena, Concert Hall, Club.
- 🧭 **Sound position** — front, front L/R, rear L/R.
- ⏻ **Auto power off** — 5 / 30 / 60 / 180 min, on-removal, or disabled.
- 👂 **Wearing detection** — pause playback when the headphones come off.
- 📶 **Link preference** — LDAC sound quality vs. stable connection.
- ⌨️ **Keyboard navigation** — vim-style (`h`/`j`/`k`/`l`, `Enter`, `Esc`) in the panel.
- 💻 **CLI (`sony-xm3-ctl`)** — everything the panel does, scriptable.
- ⚡ **No polling** — native BlueZ RFCOMM plus a file-watched state file.

---

## Noise control is one slider, not three buttons

Worth understanding before you use it, because it is the XM3's actual hardware
model rather than a UI choice:

| Step | Mode |
|---|---|
| 0 | Noise Cancelling |
| 1 | Wind Noise Reduction |
| 2 … 19 | Ambient Sound, increasing passthrough |

The mode *is* the step. `sony-xm3-ctl ambient-level 0` and
`sony-xm3-ctl noise anc` are the same command; setting an ambient level of 12
puts the headset in ambient mode at step 12. The panel presents mode buttons and
a slider over the ambient part of the range, and the daemon remembers where you
left the slider so switching ANC → Ambient returns there.

The upper bound comes from the headset itself (`NCASM_RET_CAPABILITY`), falling
back to 19 until it answers.

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   Omarchy Shell (QML)                  │
│   ┌───────────────┐ ┌─────────────┐ ┌──────────────┐   │
│   │   SonyIcon    │ │  Service    │ │    Panel     │   │
│   └───────▲───────┘ └──────▲──────┘ └──────▲───────┘   │
│           │                │               │           │
│           └────────────────┼───────────────┘           │
│                            │ watches (FileView)        │
│                 ~/.local/state/sony-xm3/               │
│                        status.json                     │
│                            ▲                           │
└────────────────────────────┼───────────────────────────┘
                             │ writes (atomic, 0600)
┌────────────────────────────┼───────────────────────────┐
│  Headless Daemon           │      Companion CLI        │
│  (sony-xm3-daemon)         │      (sony-xm3-ctl)       │
│                            │            │              │
│   UNIX Domain Socket ◄─────┴────────────┘              │
│   (/run/user/$UID/sony-xm3.sock)                       │
│                 │                                      │
│                 ▼                                      │
│   Bluetooth RFCOMM  (MDR v1 / table 1 protocol)        │
│                 │  (channel resolved over SDP)         │
│                 ▼                                      │
│       Sony WH-1000XM3                                  │
└────────────────────────────────────────────────────────┘
```

- **Plugin** (`Panel.qml`, `Service.qml`, `Model.js`, `SonyIcon.qml`) — Quickshell/QML, Omarchy manifest schema 1.
- **`daemon/`** — C++20 daemon owning the RFCOMM link and the UNIX socket.
- **`cli/`** — `sony-xm3-ctl`, a thin client for the same socket.

---

## Prerequisites

Arch / Omarchy:
```bash
sudo pacman -S --needed base-devel cmake ninja bluez bluez-libs bluez-utils dbus jq
```

Debian / Ubuntu:
```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build libbluetooth-dev libdbus-1-dev jq
```

---

## Install

```bash
git clone <this repo>
cd omarchy-sony-xm3
./setup
```

`./setup` verifies build dependencies, warns about missing Bluetooth
prerequisites, builds with CMake + Ninja, installs `sony-xm3-daemon` and
`sony-xm3-ctl` to `~/.local/bin/`, registers the `sony-xm3.service` user unit,
and deploys the plugin to `~/.config/omarchy/plugins/`.

Pair the headset first if you have not already:

```bash
bluetoothctl
  scan on
  pair    <MAC>
  trust   <MAC>
  connect <MAC>
```

---

## CLI

```bash
sony-xm3-ctl status                    # full state as JSON

# Noise control
sony-xm3-ctl noise anc                 # step 0
sony-xm3-ctl noise wind                # step 1
sony-xm3-ctl noise ambient             # last ambient step
sony-xm3-ctl noise ambient 12          # a specific ambient step
sony-xm3-ctl noise off                 # noise processing off
sony-xm3-ctl ambient-level 12          # same axis, directly
sony-xm3-ctl voice-focus on            # Focus on Voice (step 2+)

# Equalizer
sony-xm3-ctl eq vocal
sony-xm3-ctl eq custom 0 2 4 2 0 5     # 5 bands then Clear Bass, each -10..10

# Everything else
sony-xm3-ctl dsee on                   # DSEE HX
sony-xm3-ctl ear-detect on             # pause when removed
sony-xm3-ctl surround concert          # off|outdoor|arena|concert|club
sony-xm3-ctl sound-position front      # off|front-left|front-right|front|rear-left|rear-right
sony-xm3-ctl auto-power-off 180min     # off|5min|30min|60min|180min|on-remove
sony-xm3-ctl connection quality        # quality|stable
```

Exit codes: `0` success, `1` bad arguments or a daemon error, `2` daemon
unreachable.

---

## Service

```bash
systemctl --user status sony-xm3.service
journalctl --user -u sony-xm3.service -f
systemctl --user restart sony-xm3.service
```

The daemon also runs offline for development:

```bash
sony-xm3-daemon --mock --state-dir /tmp/xm3 --runtime-dir /run/user/$UID
```

---

## Tests

```bash
# C++ protocol, state and IPC unit tests
cmake -B build -G Ninja && cmake --build build
./build/daemon/test_protocol

# C++ stress and boundary suites
./build/tests/stress_test_protocol
./build/tests/stress_test_boundary_transport

# Plugin model tests (Deno or Node)
deno run --allow-read tests/model.test.js

# End-to-end simulation
./tests/integration.sh

# Python stress suites
python3 tests/stress_ipc.py
python3 tests/stress_daemon_lifecycle.py
python3 tests/challenger_stress.py
```

---

## Why this is a port, not a config change

The XM3 speaks Sony's **v1** MDR command table; the XM5 speaks **v2**. Same
framing, same checksum, different command bytes for the same features — and a v2
command sent to an XM3 is accepted and silently ignored, so the failure mode is
"nothing happens" rather than an error. The differences that matter:

| | v1 (XM3) | v2 (XM5) |
|---|---|---|
| Battery | `0x10/0x11/0x13` | `0x22/0x23/0x25` |
| NC/ASM | type `0x02`, 8-byte payload, one step axis | type `0x17`, 7-byte payload, discrete modes |
| EQ inquired type | `0x01` | `0x00` |
| Upscaling inquired type | `0x02` (DSEE HX) | `0x01` (DSEE Extreme) |
| Wearing detection | type `0x03`, ON = `0x01` | type `0x01`, ON = `0x00` |
| Speak-to-Chat | — | supported |
| Multipoint | — | supported |
| Surround / sound position | supported | — |
| Service UUID | `96CC203E-…` | `956C7B26-…` |

Speak-to-Chat and Multipoint are gone from this build because the XM3 has
neither; Surround, Sound Position, Auto Power Off and the LDAC link preference
are new because it does have those.

Full byte-level detail: [docs/protocol-v1.md](docs/protocol-v1.md).

---

## Acknowledgments

1. **[andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony)** — the XM5 project this is ported from: architecture, daemon/state/IPC design, and the QML panel.
2. **[thisisgm/omarchy-pods](https://github.com/thisisgm/omarchy-pods)** — the bar-widget + headless-daemon + atomic-state-file pattern underneath both.
3. **[Plutoberth/SonyHeadphonesClient](https://github.com/Plutoberth/SonyHeadphonesClient)** — the original XM3 client; the source for the v1 NC/ASM command shape.
4. **[mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient)** — its successor, whose generated `ProtocolV1T1.hpp` tables pinned down the rest of the v1 command set.

---

## Disclaimer

Unofficial community project for Linux desktop integration. Not affiliated with,
authorized, maintained, sponsored, or endorsed by Sony Corporation. "Sony" and
"WH-1000XM3" are trademarks of Sony Corporation.

---

## License

MIT. See [LICENSE](LICENSE).
