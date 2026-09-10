# Sony MDR protocol v1 (table 1) — WH-1000XM3

This is the byte-level reference for what `sony-xm3-daemon` puts on the wire.
It documents the **v1 / table 1** command set, which is what the WH-1000XM3
generation speaks. It is *not* the v2 set used by the WH-1000XM5.

Everything here is derived from two open-source reverse-engineering efforts:

- [Plutoberth/SonyHeadphonesClient](https://github.com/Plutoberth/SonyHeadphonesClient) — the original XM3 client (archived).
- [mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient) — its successor, whose `libmdr/include/mdr/ProtocolV1T1.hpp` is a machine-generated transcription of Sony Sound Connect's own message tables.

---

## Transport

| | |
|---|---|
| Link | Bluetooth Classic, RFCOMM |
| Service UUID | `96CC203E-5068-46AD-B32D-E316F5E069BA` |
| Channel | Resolved per device over SDP; commonly 9 |

The XM5-generation UUID is `956C7B26-D49A-4BA8-B03F-B17D393CB6E2`. The daemon
refuses to attach to a device that advertises only that one — a v2 headset will
happily accept the RFCOMM connection and then ignore every v1 command, which is
a far more confusing failure than a clear refusal.

> **The headset must be paired to this machine's own Bluetooth adapter.**
> A USB Bluetooth *audio transmitter* dongle (Avantree DG60 and similar) pairs
> with the headphones itself and presents to Linux as a USB sound card. BlueZ
> never sees the headphones, so there is no RFCOMM socket to open. The XM3 also
> holds only one host link at a time, so it cannot be on the dongle and this
> machine at once.

---

## Framing

Identical in v1 and v2:

```
0x3E  ESCAPE( <type> <seq> <len:be32> <payload…> <checksum> )  0x3C
```

| Field | Size | Notes |
|---|---|---|
| Start marker | 1 | `0x3E` (`>`) |
| Type | 1 | `0x01` ACK, `0x0C` DATA_MDR, `0x0E` DATA_MDR_NO2 |
| Sequence | 1 | Toggles `0` / `1` |
| Length | 4 | Big-endian payload length |
| Payload | *len* | Command bytes, tabled below |
| Checksum | 1 | 8-bit additive sum of every preceding unescaped byte |
| End marker | 1 | `0x3C` (`<`) |

Escaping is applied to everything between the markers, checksum included:

| Raw | Escaped |
|---|---|
| `0x3C` | `0x3D 0x2C` |
| `0x3D` | `0x3D 0x2D` |
| `0x3E` | `0x3D 0x2E` |

An ACK is a payload-less frame of type `0x01` carrying the *inverted* sequence
number of the frame being acknowledged. The daemon ACKs every inbound
`DATA_MDR` frame.

---

## Command bytes (payload byte 0)

Each family follows a `GET / RET / SET / NTFY` pattern.

| Command | Byte |
|---|---|
| `COMMON_GET_BATTERY_LEVEL` | `0x10` |
| `COMMON_RET_BATTERY_LEVEL` | `0x11` |
| `COMMON_NTFY_BATTERY_LEVEL` | `0x13` |
| `COMMON_GET_UPSCALING_EFFECT` | `0x14` |
| `COMMON_RET_UPSCALING_EFFECT` | `0x15` |
| `COMMON_NTFY_UPSCALING_EFFECT` | `0x17` |
| `COMMON_GET_AUDIO_CODEC` | `0x18` |
| `COMMON_RET_AUDIO_CODEC` | `0x19` |
| `COMMON_NTFY_AUDIO_CODEC` | `0x1B` |
| `VPT_GET/RET/SET/NTFY_PARAM` | `0x46` / `0x47` / `0x48` / `0x49` |
| `EQEBB_GET/RET/SET/NTFY_PARAM` | `0x56` / `0x57` / `0x58` / `0x59` |
| `NCASM_GET/RET_CAPABILITY` | `0x60` / `0x61` |
| `NCASM_GET/RET/SET/NTFY_PARAM` | `0x66` / `0x67` / `0x68` / `0x69` |
| `AUDIO_GET/RET/SET/NTFY_PARAM` | `0xE6` / `0xE7` / `0xE8` / `0xE9` |
| `SYSTEM_GET/RET/SET/NTFY_PARAM` | `0xF6` / `0xF7` / `0xF8` / `0xF9` |

### Where v1 and v2 differ

These are the traps when porting XM5 code to the XM3. Each one fails silently:
the headset accepts the frame and does nothing.

| Feature | v1 (XM3) | v2 (XM5) |
|---|---|---|
| Battery | `COMMON_*_BATTERY_LEVEL` `0x10/0x11/0x13` | `POWER_*_STATUS` `0x22/0x23/0x25` |
| NC/ASM inquired type | `0x02` (NC **and** ASM), 8-byte payload | `0x17` (dual-mode switch), 7-byte payload |
| EQ inquired type | `PRESET_EQ = 0x01` | `PRESET_EQ = 0x00` |
| Upscaling inquired type | `UPSCALING = 0x02` | `UPSCALING = 0x01` |
| Wearing detection | `CONTROL_BY_WEARING = 0x03`, ON = `0x01` | `PLAYBACK_CONTROL_BY_WEARING = 0x01`, ON = `0x00` |
| Speak-to-Chat | not supported | `SMART_TALKING_MODE_TYPE2 = 0x0C` |
| Multipoint | not supported | `GENERAL_SETTING1` |

---

## Noise control is one axis, not three modes

This is the single most important thing about the XM3. Where the XM5 exposes
discrete modes, the XM3 exposes **one continuous step axis**, and the mode is a
position on it:

| Step | Meaning |
|---|---|
| 0 | Noise cancelling (dual sensor) |
| 1 | Wind noise reduction (single sensor) |
| 2 … *max* | Ambient sound, increasing passthrough |

*max* is whatever `NCASM_RET_CAPABILITY` reports minus one; on the XM3 it is 19.
Focus on Voice is only accepted from step 2 upward.

Everything the daemon does to noise control funnels through one 8-byte command:

```
0x68  NCASM_SET_PARAM
0x02  NcAsmInquiredType::NOISE_CANCELLING_AND_AMBIENT_SOUND_MODE
 ..   NcAsmEffect          OFF 0x00 | ON 0x01 | ADJUST_IN_PROGRESS 0x10 | ADJUST_COMPLETE 0x11
 ..   NcAsmSettingType     ON_OFF 0x00 | LEVEL_ADJUSTMENT 0x01 | DUAL_SINGLE_OFF 0x02
 ..   NcDualSingleValue    OFF 0x00 (= ambient) | SINGLE 0x01 (= wind) | DUAL 0x02 (= full NC)
 ..   AsmSettingType       ON_OFF 0x00 | LEVEL_ADJUSTMENT 0x01
 ..   AsmId                NORMAL 0x00 | VOICE 0x01 (Focus on Voice)
 ..   asmLevel             step, or 0xFF when noise processing is off
```

So the four user-facing modes are:

| Mode | effect | ncValue | asmLevel |
|---|---|---|---|
| Noise cancelling | `0x11` | `0x02` DUAL | `0x00` |
| Wind noise reduction | `0x11` | `0x01` SINGLE | `0x01` |
| Ambient sound (level *n*) | `0x11` | `0x00` OFF | *n* |
| Off | `0x00` | `0x00` | `0xFF` |

`NCASM_RET_PARAM` (`0x67`) and `NCASM_NTFY_PARAM` (`0x69`) return the same
8-byte shape.

### Capability

```
0x61  NCASM_RET_CAPABILITY
0x02  inquired type
 ..   ncSettingType
 ..   ncStep
 ..   asmSettingType
 ..   count            number of (asmId, steps) pairs that follow
 ..   asmId, steps     repeated `count` times
```

The daemon reads the `NORMAL` entry's step *count* and stores `count - 1` as the
top usable step.

---

## Equalizer

```
0x58  EQEBB_SET_PARAM
0x01  EqEbbInquiredType::PRESET_EQ      <- 0x01 on v1, 0x00 on v2
 ..   EqPresetId
 ..   number of band steps that follow  <- 0x00 to select a preset only
 ..   [ClearBass, band0 … band4]        <- 6 bytes when set, each biased by +10
```

Band values are `-10 … +10`, sent as `value + 10` (so `0 … 20`). The array is
Clear Bass **first**, then the five bands.

| Preset | Id | | Preset | Id |
|---|---|---|---|---|
| Off | `0x00` | | Bass Boost | `0x16` |
| Bright | `0x10` | | Speech | `0x17` |
| Excited | `0x11` | | Manual (custom) | `0xA0` |
| Mellow | `0x12` | | Custom 1 | `0xA1` |
| Relaxed | `0x13` | | Custom 2 | `0xA2` |
| Vocal | `0x14` | | | |
| Treble Boost | `0x15` | | | |

---

## Battery and codec

```
0x10 0x00                 query battery
0x11 0x00 <level> <chg>   response: level 0-100, chg 0x01 while charging
0x13 …                    unsolicited notification, same shape
```

```
0x18 0x00                 query codec
0x19 0x00 <codec>         response
```

| Codec | Byte |
|---|---|
| Unsettled | `0x00` |
| SBC | `0x01` |
| AAC | `0x02` |
| LDAC | `0x10` |
| aptX | `0x20` |
| aptX HD | `0x21` |

---

## DSEE HX and link preference

Both live under `AUDIO_SET_PARAM`:

```
0xE8 0x02 0x00 <0|1>      DSEE HX: 0 = off, 1 = auto
0xE8 0x01 0x00 <0|1>      link: 0 = sound quality, 1 = stable connection
```

`COMMON_GET_UPSCALING_EFFECT` (`0x14 0x00`) reports whether upscaling is
*currently active*, which is a different question from whether the setting is
on — DSEE HX does nothing on an already-lossless source.

---

## VPT: surround and sound position

```
0x48 0x01 <preset>        surround
0x48 0x02 <position>      sound position
```

| Surround | Id | | Sound position | Id |
|---|---|---|---|---|
| Off | `0x00` | | Off | `0x00` |
| Outdoor Festival | `0x01` | | Front left | `0x01` |
| Arena | `0x02` | | Front right | `0x02` |
| Concert Hall | `0x03` | | Front | `0x03` |
| Club | `0x04` | | Rear left | `0x11` |
| | | | Rear right | `0x12` |

---

## System settings

```
0xF8 0x03 0x00 <0|1>              wearing detection: 1 = on
0xF8 0x04 0x01 <active> <select>  auto power off
```

`active` is the element in force; `select` is the timer the Sony app should
still show. Disabling therefore sends `active = 0x11` while keeping a real timer
in `select`.

| Auto power off | Id |
|---|---|
| After 5 min | `0x00` |
| After 30 min | `0x01` |
| After 60 min | `0x02` |
| After 180 min | `0x03` |
| When removed | `0x10` |
| Disabled | `0x11` |

---

## What the XM3 does not have

Present in the v1 table but not on this headset, or absent from v1 entirely:

- **Speak-to-Chat** — XM4 and later.
- **Multipoint** — XM4 (via firmware) and later. The XM3 holds one host link.
- **DSEE Extreme** — the XM3 has DSEE HX, the earlier algorithm.
- **Adaptive Sound Control** — configured on the headset by the mobile app; not
  exposed as a desktop-settable parameter here.
- **Firmware updates** — the Sony Sound Connect mobile app remains the only way.

---

## Alternatives worth knowing about

For system-wide EQ, **EasyEffects + [AutoEq](https://autoeq.app/)** profiles are
better than the headset's own five-band EQ, and they compose fine with this
project: use EasyEffects for tone, and this for the things only the headset can
do (ANC, ambient, DSEE HX, VPT).
