// plugin/Service.qml
// Reactive state and command dispatch for Sony WH-1000XM3 headphones.
import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

Item {
  id: root

  property var settings: ({})

  readonly property string cliBinary: {
    var home = Quickshell.env("HOME")
    return home ? (home + "/.local/bin/sony-xm3-ctl") : "sony-xm3-ctl"
  }

  // Path to status.json ($XDG_STATE_HOME/sony-xm3/status.json)
  readonly property string statusPath: {
    var xdg = Quickshell.env("XDG_STATE_HOME")
    var home = Quickshell.env("HOME")
    var base = xdg ? xdg : (home ? home + "/.local/state" : "/tmp")
    return base + "/sony-xm3/status.json"
  }

  // ---------------------------------------------------------------------------
  // State
  //
  // `_real` is the last status the daemon wrote. `_pending` holds values the
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
  // Command dispatch: one sony-xm3-ctl process at a time, in order.
  // ---------------------------------------------------------------------------
  property var commandQueue: []

  function runCommand(args) {
    commandQueue.push([cliBinary].concat(args))
    dispatchNext()
  }

  function dispatchNext() {
    if (ctlProcess.running || commandQueue.length === 0) return
    ctlProcess.command = commandQueue.shift()
    ctlProcess.running = true
  }

  Process {
    id: ctlProcess
    running: false
    stdout: StdioCollector { id: ctlStdout; waitForEnd: true }
    stderr: StdioCollector { id: ctlStderr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        var err = String(ctlStderr.text || ctlStdout.text || "").trim()
        if (err) root.lastError = Model.elideError(err)
      }
      dispatchNext()
    }
  }

  // Reactive FileView watcher (no polling).
  FileView {
    id: fileView
    path: root.statusPath
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onLoaded: {
      var content = typeof text === "function" ? text() : (fileView.text || "")
      root.applyStatus(content)
    }
    onLoadFailed: root.applyStatus("")
    onFileChanged: reload()
  }

  function applyStatus(raw) {
    var parsed = Model.parseStatus(raw)
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
    fileView.reload()
  }

  Component.onCompleted: fileView.reload()
}
