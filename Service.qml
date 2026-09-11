// plugin/Service.qml
// Reactive singleton / manager service for Sony WH-1000XM3 headphones.
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

  // Reactive state properties
  property bool ok: false
  property string lastError: ""
  property int schemaVersion: 0
  property bool schemaTooNew: false
  property bool connected: false
  property string deviceName: ""
  property int batteryLevel: Model.LEVEL_UNKNOWN
  property bool batteryCharging: false
  property string codec: ""
  property int ambientMaxLevel: Model.STEP_AMBIENT_MAX_DEFAULT
  property var eqCustomBands: [0, 0, 0, 0, 0]
  property int clearBass: 0
  property string autoPowerOff: "unknown"
  property string _realConnectionMode: "unknown"

  // Real internal states reported by daemon
  property string _realNoiseMode: Model.NOISE_UNKNOWN
  property int _realAmbientSoundLevel: 0
  property bool _realVoicePassthrough: false
  property string _realEqPreset: Model.EQ_OFF
  property bool _realDseeHx: false
  property bool dseeHxActive: false
  property string _realSurround: Model.SURROUND_OFF
  property string _realSoundPosition: Model.SOUND_POSITION_OFF

  // Optimistic desired states
  property string _desiredNoiseMode: ""
  property int _desiredAmbientLevel: -1
  property var _desiredVoicePassthrough: null
  property string _desiredEqPreset: ""
  property var _desiredDsee: null
  property string _desiredConnectionMode: ""
  property string _desiredSurround: ""
  property string _desiredSoundPosition: ""

  // Exposed effective properties (optimistic value if pending, otherwise real value)
  readonly property string noiseMode: _desiredNoiseMode !== "" ? _desiredNoiseMode : _realNoiseMode
  readonly property int ambientSoundLevel: _desiredAmbientLevel !== -1 ? _desiredAmbientLevel : _realAmbientSoundLevel
  readonly property bool voicePassthrough: _desiredVoicePassthrough !== null ? _desiredVoicePassthrough : _realVoicePassthrough
  readonly property string eqPreset: _desiredEqPreset !== "" ? _desiredEqPreset : _realEqPreset
  readonly property bool dseeHx: _desiredDsee !== null ? _desiredDsee : _realDseeHx
  readonly property string surround: _desiredSurround !== "" ? _desiredSurround : _realSurround
  readonly property string soundPosition: _desiredSoundPosition !== "" ? _desiredSoundPosition : _realSoundPosition
  readonly property string connectionMode: _desiredConnectionMode !== "" ? _desiredConnectionMode : _realConnectionMode

  // EQ and surround are unavailable while the headset streams LDAC.
  readonly property bool dspAvailable: Model.dspAvailable(connectionMode)

  // Focus on Voice only exists in the upper part of the ambient range.
  readonly property bool voiceFocusAvailable: Model.isVoiceFocusAvailable(noiseMode, ambientSoundLevel)

  // 4000ms optimistic state settlement timer
  Timer {
    id: settleTimer
    interval: 4000
    repeat: false
    onTriggered: {
      root.clearOptimisticOverrides()
      interval = 4000
    }
  }

  function clearOptimisticOverrides() {
    _desiredNoiseMode = ""
    _desiredAmbientLevel = -1
    _desiredVoicePassthrough = null
    _desiredEqPreset = ""
    _desiredDsee = null
    _desiredConnectionMode = ""
    _desiredSurround = ""
    _desiredSoundPosition = ""
  }

  function hasPendingOverrides() {
    return _desiredNoiseMode !== "" || _desiredAmbientLevel !== -1 ||
           _desiredVoicePassthrough !== null || _desiredEqPreset !== "" ||
           _desiredDsee !== null || _desiredConnectionMode !== "" ||
           _desiredSurround !== "" || _desiredSoundPosition !== ""
  }

  // Command dispatch queue
  property var commandQueue: []

  function runCommand(args) {
    var cmd = [cliBinary].concat(args)
    commandQueue.push(cmd)
    dispatchNext()
  }

  function dispatchNext() {
    if (ctlProcess.running || commandQueue.length === 0) return
    var nextCmd = commandQueue.shift()
    ctlProcess.command = nextCmd
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
        if (err) {
          root.lastError = Model.elideError(err)
        }
      }
      dispatchNext()
    }
  }

  // Reactive FileView watcher (zero periodic polling timers for file reading)
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
    var content = raw
    if (content === undefined || content === null) {
      content = typeof fileView.text === "function" ? fileView.text() : (fileView.text || "")
    }
    var parsed = Model.parseStatus(content)

    ok = parsed.ok === true
    lastError = parsed.lastError || ""
    schemaVersion = parsed.schemaVersion || 0
    schemaTooNew = parsed.schemaTooNew === true
    connected = parsed.connected === true
    deviceName = parsed.deviceName || ""
    batteryLevel = parsed.batteryLevel !== undefined ? parsed.batteryLevel : Model.LEVEL_UNKNOWN
    batteryCharging = parsed.batteryCharging === true
    codec = parsed.codec || ""
    ambientMaxLevel = parsed.ambientMaxLevel || Model.STEP_AMBIENT_MAX_DEFAULT
    eqCustomBands = parsed.eqCustomBands || [0, 0, 0, 0, 0]
    clearBass = parsed.clearBass !== undefined ? parsed.clearBass : 0
    autoPowerOff = parsed.autoPowerOff || "unknown"
    _realConnectionMode = parsed.connectionMode || "unknown"
    dseeHxActive = parsed.dseeHxActive === true

    _realNoiseMode = parsed.noiseMode || Model.NOISE_UNKNOWN
    _realAmbientSoundLevel = parsed.ambientSoundLevel !== undefined ? parsed.ambientSoundLevel : 0
    _realVoicePassthrough = parsed.voicePassthrough === true
    _realEqPreset = parsed.eqPreset || Model.EQ_OFF
    _realDseeHx = parsed.dseeHx === true
    _realSurround = parsed.surround || Model.SURROUND_OFF
    _realSoundPosition = parsed.soundPosition || Model.SOUND_POSITION_OFF

    // Reconcile optimistic values with settled daemon updates
    if (_desiredNoiseMode !== "" && _realNoiseMode === _desiredNoiseMode) _desiredNoiseMode = ""
    if (_desiredAmbientLevel !== -1 && _realAmbientSoundLevel === _desiredAmbientLevel) _desiredAmbientLevel = -1
    if (_desiredVoicePassthrough !== null && _realVoicePassthrough === _desiredVoicePassthrough) _desiredVoicePassthrough = null
    if (_desiredEqPreset !== "" && _realEqPreset === _desiredEqPreset) _desiredEqPreset = ""
    if (_desiredDsee !== null && _realDseeHx === _desiredDsee) _desiredDsee = null
    if (_desiredConnectionMode !== "" && _realConnectionMode === _desiredConnectionMode) _desiredConnectionMode = ""
    if (_desiredSurround !== "" && _realSurround === _desiredSurround) _desiredSurround = ""
    if (_desiredSoundPosition !== "" && _realSoundPosition === _desiredSoundPosition) _desiredSoundPosition = ""

    if (!hasPendingOverrides()) {
      settleTimer.stop()
    }
  }

  function setNoiseMode(mode) {
    if (Model.NOISE_MODES.indexOf(mode) === -1) return
    _desiredNoiseMode = mode
    // The step follows the mode, so predict it too — otherwise the slider jumps
    // to the old value for the length of one status-file round trip.
    if (mode === Model.NOISE_ANC) _desiredAmbientLevel = Model.STEP_ANC
    else if (mode === Model.NOISE_WIND) _desiredAmbientLevel = Model.STEP_WIND
    settleTimer.restart()
    runCommand(["noise", mode])
  }

  function setAmbientLevel(level) {
    var clamped = Model.clamp(level, 0, ambientMaxLevel, 0)
    _desiredAmbientLevel = clamped
    _desiredNoiseMode = Model.stepToNoiseMode(clamped)
    settleTimer.restart()
    runCommand(["ambient-level", String(clamped)])
  }

  function setVoiceFocus(enabled) {
    _desiredVoicePassthrough = enabled === true
    settleTimer.restart()
    runCommand(["voice-focus", enabled ? "on" : "off"])
  }

  function setEqPreset(preset) {
    if (!dspAvailable) return
    if (Model.EQ_PRESETS.indexOf(preset) === -1 && preset !== "user1" && preset !== "user2") return
    _desiredEqPreset = preset
    settleTimer.restart()
    runCommand(["eq", preset])
  }

  function setEqCustom(b1, b2, b3, b4, b5, cb) {
    if (!dspAvailable) return
    _desiredEqPreset = Model.EQ_CUSTOM
    settleTimer.restart()
    runCommand([
      "eq", "custom",
      String(Model.clamp(b1, -10, 10, 0)),
      String(Model.clamp(b2, -10, 10, 0)),
      String(Model.clamp(b3, -10, 10, 0)),
      String(Model.clamp(b4, -10, 10, 0)),
      String(Model.clamp(b5, -10, 10, 0)),
      String(Model.clamp(cb, -10, 10, 0))
    ])
  }

  function setDsee(enabled) {
    _desiredDsee = enabled === true
    settleTimer.restart()
    runCommand(["dsee", enabled ? "on" : "off"])
  }

  function setSurround(preset) {
    if (!dspAvailable) return
    if (Model.SURROUND_PRESETS.indexOf(preset) === -1) return
    _desiredSurround = preset
    settleTimer.restart()
    runCommand(["surround", preset])
  }

  function setSoundPosition(position) {
    if (!dspAvailable) return
    if (Model.SOUND_POSITIONS.indexOf(position) === -1) return
    _desiredSoundPosition = position
    settleTimer.restart()
    runCommand(["sound-position", position])
  }

  function setAutoPowerOff(value) {
    if (Model.AUTO_POWER_OFF_VALUES.indexOf(value) === -1) return
    runCommand(["auto-power-off", value])
  }

  function setConnectionMode(mode) {
    if (Model.CONNECTION_MODES.indexOf(mode) === -1) return
    if (mode === connectionMode) return
    _desiredConnectionMode = mode
    // The headset drops and re-establishes audio while it switches, so give
    // the optimistic value longer than usual to be confirmed.
    settleTimer.interval = 10000
    settleTimer.restart()
    runCommand(["connection", mode])
  }

  function cycleNoiseMode() {
    setNoiseMode(Model.cycleNoiseMode(noiseMode))
  }

  function refresh() {
    fileView.reload()
  }

  Component.onCompleted: {
    fileView.reload()
  }
}
