# Sony MDR protocol v1 (table 1) — WH-1000XM3

This is the byte-level reference for what `sony-xm3-daemon` puts on the wire.
It documents the **v1 / table 1** command set, which is what the WH-1000XM3
generation speaks. It is *not* the v2 set used by the WH-1000XM5.

Everything here is derived from two open-source reverse-engineering efforts:

- [Plutoberth/SonyHeadphonesClient](https://github.com/Plutoberth/SonyHeadphonesClient) — the original XM3 client (archived).
- [mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient) — its successor, whose `libmdr/include/mdr/ProtocolV1T1.hpp` is a machine-generated transcription of Sony Sound Connect's own message tables.

and then checked against a real WH-1000XM3 on 2026-09-11. Where the hardware
disagreed with the tables, or with my reading of them, the hardware wins and the
section says so. Captured frames from that session are in the unit tests.

---

## Transport

| | |
|---|---|
| Link | Bluetooth Classic, RFCOMM |
| Service UUID | `96CC203E-5068-46AD-B32D-E316F5E069BA` |
| Channel | Resolved per device over SDP — **15** on the test XM3 |

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

The XM5 code this project started from hardcodes RFCOMM channel 9. The test
XM3 had its control service on channel 15, so a hardcoded channel would never
have connected.

---

## Session handshake (required)

Before it will answer any settings query, the XM3 wants three messages, in this
order — the same opening Sony's app uses:

```
0x00 0x00     CONNECT_GET_PROTOCOL_INFO    -> 01 00 40 10   (protocol 0x4010)
0x02 0x00     CONNECT_GET_CAPABILITY_INFO  -> 03 00 …        (includes the MAC as text)
0x06 0x00     CONNECT_GET_SUPPORT_FUNCTION -> 07 00 <n> <function ids…>
```

Without them the headset still **ACKs** every query — so the link looks healthy —
but never replies. The one exception observed was `NCASM_GET_CAPABILITY`, which
it answers either way; that is why a missing handshake can pass for a partly
working daemon.

The support-function list from the test XM3:

| Id | Function | | Id | Function |
|---|---|---|---|---|
| `0x11` | Battery level | | `0x42` | Sound position |
| `0x12` | Upscaling indicator | | `0x51` | Preset EQ |
| `0x13` | Codec indicator | | `0x71` | Adaptive Sound Control |
| `0x14` | BLE setup | | `0x81` | NC Optimizer |
| `0x30` | Firmware update | | `0xa1` | Playback controller |
| `0x39` | Voice guidance | | `0xc1` | Action log |
| `0x41` | VPT (surround) | | `0xd1`, `0xd2` | General settings 1, 2 |
| `0x62` | NC + ambient sound | | `0xe1` | Connection mode |
| `0xe2` | Upscaling | | `0xf4` | Auto power off |

Notably absent: `0xf3` (control by wearing). The XM3 has no wearing sensor.

---

## Flow control

**One command in flight.** Send a `DATA_MDR` frame, then wait for the headset's
ACK before sending the next. Anything sent before that ACK is silently dropped —
send eleven queries back to back and exactly one gets answered.

**The next sequence number comes from the ACK, and only the ACK.** The headset
numbers its own `DATA` frames (replies and notifications such as volume changes)
independently. Taking the sequence number from those — which one reference
client does — puts the next command out of step, and it is only accepted on the
retry after a one-second timeout.

**On an ACK timeout, flip the sequence bit and resend.** That is how Sony's
client recovers from a lost ACK, and it is what rescued the out-of-step commands
above.

Replies usually arrive *after* the ACK, so the reply to one query tends to land
just after the next query has gone out. That is normal.

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
| Wearing detection | not on the XM3 (no sensor) | `PLAYBACK_CONTROL_BY_WEARING = 0x01`, ON = `0x00` |
| Session handshake | required before settings queries | — |
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
8-byte shape — all four modes were confirmed this way on hardware. The headset
normalises two fields in what it reports back: `effect` comes back as `0x01`
rather than the `0x11` sent, and `ncSettingType` as `0x02` rather than `0x01`.
So `68 02 11 01 00 01 00 0c` (ambient 12) is confirmed as
`69 02 01 02 00 01 00 0c`. A decoder should key off `ncValue`, not those two.

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

`COMMON_GET_UPSCALING_EFFECT` (`0x14 0x00`, notified as `0x17`) reports whether
upscaling is *currently active*, which is a different question from whether the
setting is on:

```
17 00 00 01     DSEE HX processing
17 00 00 02     DSEE HX switched on but not processing
```

The headset reports `02` on LDAC, and also while EQ or surround is active. Treat
it as a status light, never as the setting — reading it as the setting makes the
toggle flip itself off the moment you switch to LDAC.

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

## LDAC blocks EQ and VPT

On "Priority on sound quality" (`e8 01 00 00`, LDAC) the XM3 cannot run its EQ
or VPT processing. An EQ, surround or sound-position command sent in that mode
is ACKed, not applied, and answered with an alert:

```
99 01 01 01     ALERT_NTFY_PARAM
                  type    0x01  FIXED_MESSAGE
                  message 0x01  DISCONNECT_CAUSED_BY_CONNECTION_MODE_CHANGE
                  action  0x01  POSITIVE_NEGATIVE
```

i.e. "this needs the connection mode changed, which will disconnect — proceed?"
Sony's app shows that as a dialog and replies with `ALERT_SET_PARAM`
(`98 01 01 <0|1>`). The daemon never provokes it: it refuses EQ and VPT
commands while the headset is on LDAC and says why.

Switching to "stable connection" (`e8 01 00 01`) drops to **SBC** — not AAC —
and EQ and VPT then work immediately. Either switch briefly drops the audio link
while the headset renegotiates.

---

## System settings

```
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
| Disabled | `0x11` |

The table also defines "when removed" (`0x10`). The XM3 ignores it — it answers
by reporting its existing timer — because it has no wearing sensor.

---

## Device info

```
04 01     CONNECT_GET_DEVICE_INFO / MODEL_NAME  -> 05 01 0a "WH-1000XM3"
04 02     CONNECT_GET_DEVICE_INFO / FW_VERSION  -> 05 02 05 "4.5.2"
```

The model name is a better display name than BlueZ's, which drifts to the BLE
advertisement's `LE_WH-1000XM3`.

---

## NC Optimizer

```
80 01                  capability  -> 81 01 02 01 09 01 02
                                      (personal fit ~9 s, barometric ~2 s)
84 01 00 01            start           84 01 00 00   cancel
82 01 / 85 …           status: 85 01 <common> <state>
86 01 / 89 …           result: 87 01 <personalType> <personal> <baroType> <baro>
```

| State byte | Meaning |
|---|---|
| `00` | idle |
| `01` | measuring fit (plays test tones) |
| `02` | measuring atmospheric pressure |
| `10` | optimizing |
| `11` | done |

The barometric value `07`…`0a` is the measured pressure, 0.7…1.0 atm. While it
runs, the headset suspends noise control (`65 02 01`) and restores it after
(`65 02 00`), so noise-mode commands in the meantime are pointless. The headset
must be worn — it measures the fit acoustically.

---

## Playback controller

```
a0 01                  capability  -> a1 01 1f 01 01   (31 volume steps: 0..30)
a6 01 20               volume      -> a7 01 20 <vol>   (notified as a9 01 20 <vol>)
a8 01 20 <vol>         set volume
a4 01 00 <ctl>         transport: 01 pause, 07 play, 02 next, 03 previous
```

This is the headset's AVRCP absolute volume, i.e. the same volume PipeWire drives
when it uses hardware volume. Play state (`a3`/`a5`) is always reported as
"unsettled" on the XM3, so the panel offers separate play and pause buttons
rather than a toggle.

---

## General settings

Generic numbered slots, which the headset names itself in its capability reply:

| Slot | Name reported | Type | Values |
|---|---|---|---|
| `d1` | `ASSIGNABLE_KEY_SETTING` | list | 0 noise control, 1 Google Assistant, 2 Amazon Alexa |
| `d2` | `TOUCH_PANEL_SETTING` | boolean | `01` on, `00` off |

```
d6 <slot>                    read   -> d7 <slot> <type> <value>
d8 <slot> <type> <value>     write  (notified as d9 …)
```

Booleans are ON = `01` on the XM3; the XM5 table inverts this. Writing the value
a setting already has is ACKed but not notified. Reassigning the button can
raise alert `99 01 02 01` (key-assign change will disconnect); the daemon answers
it yes only for a change the user just asked for.

---

## Voice guidance (table 2)

Table 2 is sent as `DATA_MDR_NO2` frames (type `0x0e`). **Its command bytes
collide with table 1's** — `47` is a surround reply in table 1 and a voice-guidance
reply in table 2 — so the frame type has to be carried all the way to the parser.

```
40 01          capability -> 41 01 <on/off switchable> <language switchable> <n> <languages…>
46 01 01       on/off     -> 47 01 01 <0|1>          set: 48 01 01 <0|1>
46 01 02       language   -> 47 01 02 <language id>  (01 = English)
```

Changing the language needs a voice-pack download, so it is not offered.

---

## Alerts

`99 01 <message> 01` is the headset asking "proceed?". The reply is
`98 01 <message> <01 yes | 00 no>`. Seen on the XM3: `01` (connection-mode change,
when EQ/VPT is sent on LDAC) and `02` (NC button reassignment). The daemon
declines anything the user did not explicitly ask for.

---

## Adaptive Sound Control

`70 01` → `71 01 01`: supported, but the protocol side is only `SENSE_SET_STATUS`
(`74`) — a flag. The activity detection that drives it runs in Sony's phone app on
the phone's sensors, so there is nothing for a desktop to drive.

---

## What the XM3 does not have

Present in the v1 table but not on this headset, or absent from v1 entirely:

- **Speak-to-Chat** — XM4 and later.
- **Multipoint** — XM4 (via firmware) and later. The XM3 holds one host link.
- **Wearing detection** — no sensor; `CONTROL_BY_WEARING` never answers, and the
  "power off when removed" timer is ignored.
- **DSEE Extreme** — the XM3 has DSEE HX, the earlier algorithm.
- **Adaptive Sound Control** as a headset feature — see above; it lives in the phone app.
- **Firmware updates** — the Sony Sound Connect mobile app remains the only way.

---

## Alternatives worth knowing about

For system-wide EQ, **EasyEffects + [AutoEq](https://autoeq.app/)** profiles are
better than the headset's own five-band EQ, and they compose fine with this
project: use EasyEffects for tone, and this for the things only the headset can
do (ANC, ambient, DSEE HX, VPT).
