// plugin/Service.qml
// Reactive state and command dispatch for Sony WH-1000XM3 headphones.
//
// The widget runs inside the long-lived shell process, so it takes the
// narrowest path it can to the daemon: one UNIX socket, and nothing else.
//
//   - It starts no processes. There is no executable path to resolve, nothing
//     is looked up through PATH, and a replaced binary somewhere in the user's
//     path cannot be run from here.
//   - It reads no files. The daemon pushes its state over the same socket
//     after `subscribe`, so no state file is opened by the shell.
//   - The socket path is pinned to this login session's runtime directory
//     (/run/user/<uid>), which the kernel creates and only this user may
//     enter. Anything else, including an unset or relative XDG_RUNTIME_DIR,
//     is refused rather than fallen back on.
//   - Everything read is bounded: a line longer than maxLineBytes, or a
//     daemon that stops answering within responseTimeoutMs, drops the
//     connection instead of growing the shell's memory or holding the queue.
pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

Item {
  id: root

  property var settings: ({})

  // ---------------------------------------------------------------------------
  // Connection to the daemon
  // ---------------------------------------------------------------------------

  // The per-user runtime directory, accepted only in its expected shape.
  readonly property string runtimeDir: {
    var dir = String(Quickshell.env("XDG_RUNTIME_DIR") || "")
    while (dir.length > 1 && dir.charAt(dir.length - 1) === "/") {
      dir = dir.substring(0, dir.length - 1)
    }
    return /^\/run\/user\/[0-9]+$/.test(dir) ? dir : ""
  }
  readonly property bool socketPathTrusted: runtimeDir !== ""
  readonly property string socketPath: socketPathTrusted ? runtimeDir + "/sony-xm3.sock" : ""

  readonly property int maxLineBytes: 65536      // a status line is well under 4 KiB
  readonly property int maxCommandBytes: 512
  readonly property int maxOutstanding: 32
  readonly property int responseTimeoutMs: 5000
  readonly property int maxBackoffMs: 10000

  property int _outstanding: 0
  property int _backoffMs: 1000

  // ---------------------------------------------------------------------------
  // State
  //
  // `_real` is the last status the daemon sent. `_pending` holds values the
  // user just chose, each with its own expiry, so a control reflects a click
  // immediately and falls back to the truth if the headset never confirms it.
  // Both objects are replaced rather than mutated so that bindings notice.
  // ---------------------------------------------------------------------------
  property var _real: Model.defaultStatus()
  property var _pending: ({})

  function _value(key) {
    return _pending.hasOwnProperty(key) ? _pending[key].value : _real[key]
  }

  function _setPending(key, value, ttlMs) {
    var next = Object.assign({}, _pending)
    next[key] = { value: value, until: Date.now() + (ttlMs || 4000) }
    _pending = next
    pendingTimer.start()
  }

  // Report-only fields: nothing the user sets directly.
  readonly property bool ok: _real.ok === true
  readonly property string lastErrorFromStatus: _real.lastError || ""
  readonly property bool schemaTooNew: _real.schemaTooNew === true
  readonly property bool connected: _real.connected === true
  readonly property string deviceName: _real.deviceName || ""
  readonly property int batteryLevel: _real.batteryLevel
  readonly property bool batteryCharging: _real.batteryCharging === true
  readonly property string codec: _real.codec || ""
  readonly property string firmwareVersion: _real.firmwareVersion || ""
  readonly property int ambientMaxLevel: _real.ambientMaxLevel || Model.STEP_AMBIENT_MAX_DEFAULT
  readonly property bool dseeHxActive: _real.dseeHxActive === true
  readonly property string optimizerPressure: _real.optimizerPressure || ""
  readonly property int volumeMax: _real.volumeMax || 30
  readonly property string voiceGuidanceLanguage: _real.voiceGuidanceLanguage || ""

  // Settable fields: pending value if there is one, otherwise the daemon's.
  readonly property string noiseMode: _value("noiseMode")
  readonly property int ambientSoundLevel: _value("ambientSoundLevel")
  readonly property bool voicePassthrough: _value("voicePassthrough") === true
  readonly property string connectionMode: _value("connectionMode")
  readonly property string eqPreset: _value("eqPreset")
  readonly property var eqCustomBands: _value("eqCustomBands") || [0, 0, 0, 0, 0]
  readonly property int clearBass: _value("clearBass") || 0
  readonly property string surround: _value("surround")
  readonly property string soundPosition: _value("soundPosition")
  readonly property bool dseeHx: _value("dseeHx") === true
  readonly property string optimizerState: _value("optimizerState")
  readonly property int volume: _value("volume")
  readonly property string ncButton: _value("ncButton")
  readonly property bool touchPanel: _value("touchPanel") === true
  readonly property bool voiceGuidance: _value("voiceGuidance") === true
  readonly property string autoPowerOff: _value("autoPowerOff")

  // Derived rules.
  readonly property bool dspAvailable: Model.dspAvailable(connectionMode)
  readonly property bool voiceFocusAvailable: Model.isVoiceFocusAvailable(noiseMode, ambientSoundLevel)
  readonly property bool optimizerRunning: Model.isOptimizerRunning(optimizerState)
  readonly property bool customEqSelected: Model.isCustomEqSlot(eqPreset)

  property string lastError: ""

  // Drops pending values once they expire; idle whenever nothing is pending.
  Timer {
    id: pendingTimer
    interval: 500
    repeat: true
    onTriggered: {
      var now = Date.now()
      var next = {}
      var kept = 0
      for (var key in root._pending) {
        if (root._pending[key].until > now) {
          next[key] = root._pending[key]
          kept++
        }
      }
      root._pending = next
      if (kept === 0) stop()
    }
  }

  // ---------------------------------------------------------------------------
  // Socket
  //
  // Each attempt gets a brand new Socket. Quickshell keeps its own target
  // state, so re-setting `connected` on a socket whose attempt already failed
  // does nothing; recreating the object makes every retry a real attempt.
  // ---------------------------------------------------------------------------
  Component {
    id: linkComponent

    Socket {
      id: sock
      path: root.socketPath
      connected: true

      parser: SplitParser {
        splitMarker: "\n"
        onRead: function(data) { root._onLine(data) }
      }

      // The socket connects while the Loader is still constructing it, so the
      // handlers pass themselves rather than going through `linkLoader.item`,
      // which is only published once construction finishes.
      onConnectionStateChanged: root._onLinkState(sock)
      // The parameter is a QLocalSocket error enum, and the reason does not
      // change what we do: back off and try again with a fresh socket.
      onError: root._onLinkLost("Cannot reach the Sony daemon")
    }
  }

  Loader {
    id: linkLoader
    active: false
    sourceComponent: linkComponent
  }

  readonly property bool linked: linkLoader.item ? linkLoader.item.connected === true : false

  function _write(text) {
    var sock = linkLoader.item
    if (!sock || !sock.connected) return false
    sock.write(text)
    sock.flush()
    return true
  }

  function _onLinkState(sock) {
    if (sock && sock.connected) {
      root._outstanding = 0
      root._backoffMs = 1000
      root.lastError = ""
      // Ask for the state now and on every change, so nothing polls and no
      // file is read.
      sock.write("subscribe\n")
      sock.flush()
      handshake.restart()
    } else {
      root._onLinkLost("The Sony daemon is not running")
    }
  }

  function _onLinkLost(reason) {
    deadline.stop()
    handshake.stop()
    root._outstanding = 0
    root._offline(reason)
    root._scheduleReconnect()
  }

  // A daemon that goes quiet is dropped rather than left holding the queue:
  // `handshake` covers the first status after subscribing, `deadline` covers
  // commands still waiting to be acknowledged.
  Timer {
    id: handshake
    interval: root.responseTimeoutMs
    repeat: false
    onTriggered: root._giveUp("The Sony daemon did not send its state")
  }

  Timer {
    id: deadline
    interval: root.responseTimeoutMs
    repeat: false
    onTriggered: root._giveUp("The Sony daemon stopped responding")
  }

  // Drops a wedged daemon: the socket object goes away, so nothing it might
  // still send can reach the shell, and a fresh attempt is scheduled.
  function _giveUp(reason) {
    root.lastError = reason
    root._onLinkLost(reason)
  }

  Timer {
    id: reconnectTimer
    interval: root._backoffMs
    repeat: false
    onTriggered: root._connect()
  }

  function _connect() {
    if (!root.socketPathTrusted) {
      root._offline("XDG_RUNTIME_DIR is not this session's runtime directory")
      return
    }
    if (root.linked) return
    linkLoader.active = false
    linkLoader.active = true
  }

  function _scheduleReconnect() {
    if (!root.socketPathTrusted) return
    // Tearing the socket down from inside its own signal handler is not safe,
    // so it happens once the current handler has returned.
    Qt.callLater(function() { linkLoader.active = false })
    reconnectTimer.interval = root._backoffMs
    root._backoffMs = Math.min(root._backoffMs * 2, root.maxBackoffMs)
    reconnectTimer.restart()
  }

  // Everything the widget shows falls back to "unknown" while there is no link.
  function _offline(reason) {
    var blank = Model.defaultStatus()
    blank.lastError = reason
    root._real = blank
    root._pending = ({})
  }

  function _onLine(data) {
    var line = String(data)
    if (line.length > root.maxLineBytes) {
      root._giveUp("The Sony daemon sent an oversized reply")
      return
    }
    line = line.trim()
    if (!line) return

    if (line.charAt(0) === "{") {
      // Either the state pushed after a change, or the answer to `status`.
      // Neither is acknowledged, so only the handshake timer is cleared.
      handshake.stop()
      root._applyStatus(line)
      return
    }

    if (line.indexOf("ERR") === 0) {
      root.lastError = Model.elideError(line.substring(3).trim())
    }
    // Anything else is the "OK" that acknowledges one command.
    if (root._outstanding > 0) root._outstanding--
    if (root._outstanding > 0) deadline.restart()
    else deadline.stop()
  }

  function _applyStatus(text) {
    var parsed = Model.parseStatus(text)
    _real = parsed

    // A pending value the headset has now confirmed is no longer pending.
    var next = {}
    for (var key in _pending) {
      if (JSON.stringify(parsed[key]) !== JSON.stringify(_pending[key].value)) {
        next[key] = _pending[key]
      }
    }
    _pending = next
  }

  // ---------------------------------------------------------------------------
  // Command dispatch
  // ---------------------------------------------------------------------------
  // `expectsAck` is false for `status`, whose reply is a state line rather
  // than an acknowledgement.
  function runCommand(args, expectsAck) {
    if (!root.linked) {
      lastError = "The Sony daemon is not running"
      _connect()
      return false
    }
    if (_outstanding >= maxOutstanding) {
      lastError = "The Sony daemon is not keeping up"
      return false
    }
    var line = args.join(" ")
    if (line.length > maxCommandBytes || line.indexOf("\n") !== -1) {
      lastError = "Refusing to send a malformed command"
      return false
    }
    if (!_write(line + "\n")) {
      lastError = "The Sony daemon is not running"
      return false
    }
    if (expectsAck !== false) {
      _outstanding++
      deadline.restart()
    }
    return true
  }

  // ---------------------------------------------------------------------------
  // Setters
  // ---------------------------------------------------------------------------
  function setNoiseMode(mode) {
    if (Model.NOISE_MODES.indexOf(mode) === -1 || optimizerRunning) return
    _setPending("noiseMode", mode)
    // The step follows the mode, so predict it too.
    if (mode === Model.NOISE_ANC) _setPending("ambientSoundLevel", Model.STEP_ANC)
    else if (mode === Model.NOISE_WIND) _setPending("ambientSoundLevel", Model.STEP_WIND)
    runCommand(["noise", mode])
  }

  function setAmbientLevel(level) {
    if (optimizerRunning) return
    var clamped = Model.clamp(level, 0, ambientMaxLevel, 0)
    _setPending("ambientSoundLevel", clamped)
    _setPending("noiseMode", Model.stepToNoiseMode(clamped))
    runCommand(["ambient-level", String(clamped)])
  }

  function setVoiceFocus(enabled) {
    _setPending("voicePassthrough", enabled === true)
    runCommand(["voice-focus", enabled ? "on" : "off"])
  }

  function setConnectionMode(mode) {
    if (Model.CONNECTION_MODES.indexOf(mode) === -1 || mode === connectionMode) return
    // The headset drops and re-establishes audio while it switches.
    _setPending("connectionMode", mode, 12000)
    runCommand(["connection", mode])
  }

  function setEqPreset(preset) {
    if (Model.EQ_PRESETS.indexOf(preset) === -1 || !dspAvailable) return
    _setPending("eqPreset", preset)
    runCommand(["eq", preset])
  }

  // Sets the bands of a custom slot (Manual, Custom 1 or Custom 2).
  function setEqBands(slot, bands, cb) {
    if (!Model.isCustomEqSlot(slot) || !dspAvailable) return
    var clean = []
    for (var i = 0; i < 5; i++) clean.push(Model.clamp(bands[i], -10, 10, 0))
    var clearBassValue = Model.clamp(cb, -10, 10, 0)
    _setPending("eqPreset", slot)
    _setPending("eqCustomBands", clean)
    _setPending("clearBass", clearBassValue)
    runCommand(["eq", slot].concat(clean.map(String)).concat([String(clearBassValue)]))
  }

  function setSurround(preset) {
    if (Model.SURROUND_PRESETS.indexOf(preset) === -1 || !dspAvailable) return
    _setPending("surround", preset)
    runCommand(["surround", preset])
  }

  function setSoundPosition(position) {
    if (Model.SOUND_POSITIONS.indexOf(position) === -1 || !dspAvailable) return
    _setPending("soundPosition", position)
    runCommand(["sound-position", position])
  }

  function setDsee(enabled) {
    _setPending("dseeHx", enabled === true)
    runCommand(["dsee", enabled ? "on" : "off"])
  }

  function startOptimizer() {
    if (optimizerRunning) return
    // Measuring takes about a dozen seconds; progress arrives as notifications.
    _setPending("optimizerState", "measuring-fit", 2500)
    runCommand(["optimizer", "start"])
  }

  function cancelOptimizer() {
    _setPending("optimizerState", "idle", 2500)
    runCommand(["optimizer", "cancel"])
  }

  function setVolume(level) {
    var clamped = Model.clamp(level, 0, volumeMax, 0)
    _setPending("volume", clamped)
    runCommand(["volume", String(clamped)])
  }

  function playback(action) {
    if (Model.PLAYBACK_ACTIONS.indexOf(action) === -1) return
    runCommand(["playback", action])
  }

  function setNcButton(button) {
    if (Model.NC_BUTTONS.indexOf(button) === -1 || button === ncButton) return
    // Reassigning the button can make the headset reconnect.
    _setPending("ncButton", button, 12000)
    runCommand(["nc-button", button])
  }

  function setTouchPanel(enabled) {
    _setPending("touchPanel", enabled === true)
    runCommand(["touch-panel", enabled ? "on" : "off"])
  }

  function setVoiceGuidance(enabled) {
    _setPending("voiceGuidance", enabled === true)
    runCommand(["voice-guidance", enabled ? "on" : "off"])
  }

  function setAutoPowerOff(value) {
    if (Model.AUTO_POWER_OFF_VALUES.indexOf(value) === -1) return
    _setPending("autoPowerOff", value)
    runCommand(["auto-power-off", value])
  }

  function cycleNoiseMode() {
    setNoiseMode(Model.cycleNoiseMode(noiseMode))
  }

  function refresh() {
    if (root.linked) runCommand(["status"], false)
    else _connect()
  }

  Component.onCompleted: _connect()
}
