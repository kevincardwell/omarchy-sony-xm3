// plugin/Panel.qml
// Omarchy bar widget and control panel for the Sony WH-1000XM3.
import QtQuick
import QtQuick.Controls
import Quickshell.Io
import qs.Commons
import qs.Ui
import "Model.js" as Model

Panel {
  id: root
  moduleName: "io.github.kevincardwell.omasonyxm3"
  ipcTarget: "io.github.kevincardwell.omasonyxm3"
  manageIpc: false

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color urgent: bar ? bar.urgent : Color.urgent
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property color muted: Qt.darker(root.foreground, 1.4)

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  Service {
    id: sony
    settings: root.settings
  }

  // ---------------------------------------------------------------------------
  // Tabs and keyboard navigation
  //
  // Each focusable section is either a toggle, a slider adjusted with h/l, or a
  // row/grid of choices with a cursor index. `cursorIndex` holds those indices;
  // it is replaced, never mutated, so that bindings reading it update.
  // ---------------------------------------------------------------------------
  property string currentTab: "sound"
  property string focusSection: "tabs"
  property bool cursorActive: false
  property var cursorIndex: ({})

  readonly property var tabs: ["sound", "device"]

  // Choice sections: their values (in display order) and grid width.
  readonly property var choiceSections: ({
    tabs: { values: root.tabs, cols: 2 },
    noise: { values: Model.NOISE_MODES, cols: 4 },
    connection: { values: Model.CONNECTION_MODES, cols: 2 },
    eq: { values: Model.EQ_PRESETS, cols: 4 },
    surround: { values: Model.SURROUND_PRESETS, cols: 5 },
    position: { values: Model.SOUND_POSITIONS, cols: 3 },
    playback: { values: Model.PLAYBACK_ACTIONS, cols: 4 },
    ncButton: { values: Model.NC_BUTTONS, cols: 3 },
    autoPowerOff: { values: Model.AUTO_POWER_OFF_VALUES, cols: 5 }
  })

  readonly property var sectionOrder: currentTab === "sound"
    ? ["tabs", "noise", "ambient", "voiceFocus", "connection", "eq", "surround", "position", "dsee"]
    : ["tabs", "volume", "playback", "optimizer", "ncButton", "touchPanel", "voiceGuidance", "autoPowerOff"]

  function idx(name) { return cursorIndex[name] || 0 }

  function setIdx(name, value) {
    var next = Object.assign({}, cursorIndex)
    next[name] = value
    cursorIndex = next
  }

  // Where the cursor sits in a choice section, or -1 when it is elsewhere.
  function cursorIn(name) {
    return root.cursorActive && root.focusSection === name ? idx(name) : -1
  }

  function syncCursorToState() {
    var current = {
      tabs: currentTab, noise: sony.noiseMode, connection: sony.connectionMode,
      eq: sony.eqPreset, surround: sony.surround, position: sony.soundPosition,
      ncButton: sony.ncButton, autoPowerOff: sony.autoPowerOff
    }
    var next = {}
    for (var name in choiceSections) {
      var i = choiceSections[name].values.indexOf(current[name])
      next[name] = i !== -1 ? i : 0
    }
    cursorIndex = next
  }

  function showTab(tab) {
    if (root.tabs.indexOf(tab) === -1) return
    currentTab = tab
    setIdx("tabs", root.tabs.indexOf(tab))
    if (panelFlick) panelFlick.contentY = 0
  }

  onOpenedChanged: {
    if (opened) {
      cursorActive = true
      focusSection = "tabs"
      syncCursorToState()
      if (panelFlick) panelFlick.contentY = 0
      sony.refresh()
      Qt.callLater(function() { keyCatcher.forceActiveFocus() })
    }
  }

  // A section is skipped by keyboard navigation when it cannot act right now.
  function sectionEnabled(name) {
    if (name === "noise") return !sony.optimizerRunning
    if (name === "ambient") return sony.noiseMode === Model.NOISE_AMBIENT && !sony.optimizerRunning
    if (name === "voiceFocus") return sony.voiceFocusAvailable
    if (name === "eq" || name === "surround" || name === "position") return sony.dspAvailable
    return true
  }

  function moveSection(dy) {
    var i = sectionOrder.indexOf(focusSection)
    if (i === -1) i = 0
    var next = i
    while (true) {
      next += (dy > 0 ? 1 : -1)
      if (next < 0 || next >= sectionOrder.length) return
      if (sectionEnabled(sectionOrder[next])) {
        focusSection = sectionOrder[next]
        return
      }
    }
  }

  function moveCursor(dx, dy) {
    cursorActive = true
    var choice = choiceSections[focusSection]

    if (dy !== 0) {
      // Multi-row grids: j/k walks within the grid before leaving it.
      if (choice && choice.cols < choice.values.length) {
        var target = idx(focusSection) + dy * choice.cols
        if (target >= 0 && target < choice.values.length) {
          setIdx(focusSection, target)
          return
        }
      }
      moveSection(dy)
      return
    }

    if (focusSection === "tabs") {
      showTab(root.tabs[Math.max(0, Math.min(root.tabs.length - 1, idx("tabs") + dx))])
    } else if (choice) {
      setIdx(focusSection, Math.max(0, Math.min(choice.values.length - 1, idx(focusSection) + dx)))
    } else if (focusSection === "ambient") {
      var lo = Model.STEP_AMBIENT_MIN
      sony.setAmbientLevel(Model.clamp(sony.ambientSoundLevel + dx, lo, sony.ambientMaxLevel, lo))
    } else if (focusSection === "volume") {
      sony.setVolume(Model.clamp(sony.volume + dx, 0, sony.volumeMax, 0))
    }
  }

  function activateCursor() {
    var choice = choiceSections[focusSection]
    if (choice) {
      choose(focusSection, choice.values[idx(focusSection)])
      return
    }
    if (focusSection === "voiceFocus") sony.setVoiceFocus(!sony.voicePassthrough)
    else if (focusSection === "dsee") sony.setDsee(!sony.dseeHx)
    else if (focusSection === "optimizer") toggleOptimizer()
    else if (focusSection === "touchPanel") sony.setTouchPanel(!sony.touchPanel)
    else if (focusSection === "voiceGuidance") sony.setVoiceGuidance(!sony.voiceGuidance)
  }

  // One place that turns a choice into a command, for clicks and keys alike.
  function choose(section, value) {
    focusSection = section
    var values = choiceSections[section].values
    setIdx(section, Math.max(0, values.indexOf(value)))
    if (section === "tabs") showTab(value)
    else if (section === "noise") sony.setNoiseMode(value)
    else if (section === "connection") sony.setConnectionMode(value)
    else if (section === "eq") sony.setEqPreset(value)
    else if (section === "surround") sony.setSurround(value)
    else if (section === "position") sony.setSoundPosition(value)
    else if (section === "playback") sony.playback(value)
    else if (section === "ncButton") sony.setNcButton(value)
    else if (section === "autoPowerOff") sony.setAutoPowerOff(value)
  }

  function toggleOptimizer() {
    if (sony.optimizerRunning) sony.cancelOptimizer()
    else sony.startOptimizer()
  }

  IpcHandler {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): string { sony.refresh(); return "ok" }
    function cycleNoise(): string { sony.cycleNoiseMode(); return sony.noiseMode }
    function showTab(tab: string): string { root.showTab(tab); return root.currentTab }
  }

  // ---------------------------------------------------------------------------
  // Reusable pieces
  // ---------------------------------------------------------------------------

  // A row or grid of mutually exclusive choices built from stock Buttons.
  // Inline components cannot see this file's ids, so everything they need is
  // passed in: `cursor` is the index to draw the keyboard cursor on (-1 for
  // none) and `chosen` reports clicks back out.
  component ChoiceGrid: Grid {
    id: grid
    property var options: []          // [{ value, label, icon? }]
    property string current: ""
    property int cursor: -1
    property bool active: true
    property color foreground: Color.foreground
    property string fontFamily: Style.font.family
    property real fontSize: Style.font.caption
    signal chosen(string value)

    spacing: Style.space(6)
    opacity: active ? 1.0 : 0.4
    readonly property real cellWidth: (width - spacing * (columns - 1)) / columns

    Repeater {
      model: grid.options

      Button {
        required property var modelData
        required property int index
        width: grid.cellWidth
        text: modelData.label
        iconText: modelData.icon || ""
        iconSize: Style.font.title
        fontSize: grid.fontSize
        foreground: grid.foreground
        fontFamily: grid.fontFamily
        bordered: true
        enabled: grid.active
        selected: grid.current === modelData.value
        hasCursor: grid.cursor === index
        horizontalPadding: Style.space(4)
        verticalPadding: Style.space(6)
        onClicked: grid.chosen(modelData.value)
      }
    }
  }

  // Header text on the left, a value on the right.
  component HeaderRow: Item {
    id: headerRow
    property string title: ""
    property string value: ""
    property color foreground: Color.foreground
    property string fontFamily: Style.font.family
    height: Math.max(headerTitle.implicitHeight, headerValue.implicitHeight)

    PanelSectionHeader {
      id: headerTitle
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      text: headerRow.title
      foreground: headerRow.foreground
      fontFamily: headerRow.fontFamily
    }

    Text {
      id: headerValue
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      textFormat: Text.PlainText
      text: headerRow.value
      color: headerRow.foreground
      font.family: headerRow.fontFamily
      font.pixelSize: Style.font.caption
      font.bold: true
    }
  }

  // One EQ band: label, slider, value. Sends only on release; the value label
  // follows the drag so there is feedback without a flood of commands.
  component BandSlider: Item {
    id: band
    property string label: ""
    property int value: 0
    property var barRef: null
    property color foreground: Color.foreground
    property string fontFamily: Style.font.family
    signal committed(int value)
    height: bandSlider.implicitHeight > 0 ? Math.max(bandSlider.implicitHeight, bandLabel.implicitHeight) : Style.space(24)

    Text {
      id: bandLabel
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(64)
      textFormat: Text.PlainText
      text: band.label
      color: band.foreground
      font.family: band.fontFamily
      font.pixelSize: Style.font.caption
    }

    PanelSlider {
      id: bandSlider
      anchors.left: bandLabel.right
      anchors.right: bandValue.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      minimum: -10
      maximum: 10
      step: 1
      integer: true
      value: band.value
      bar: band.barRef
      onReleased: function(v) { band.committed(Math.round(v)) }
    }

    Text {
      id: bandValue
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(28)
      horizontalAlignment: Text.AlignRight
      textFormat: Text.PlainText
      readonly property int shown: Math.round(bandSlider.dragging ? bandSlider.liveValue : band.value)
      text: (shown > 0 ? "+" : "") + shown
      color: band.foreground
      font.family: band.fontFamily
      font.pixelSize: Style.font.caption
      font.bold: true
    }
  }

  component Caption: Text {
    width: parent ? parent.width : 0
    textFormat: Text.PlainText
    wrapMode: Text.WordWrap
    font.pixelSize: Style.font.caption
  }

  // ---------------------------------------------------------------------------
  // Bar widget button
  // ---------------------------------------------------------------------------
  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    labelVisible: false
    hasVisualContent: true
    fixedWidth: vertical ? -1 : (contentRow.implicitWidth + scaledHorizontalMargin * 2)
    tooltipText: sony.connected
      ? ((sony.deviceName || "WH-1000XM3") + " (" + Model.noiseModeName(sony.noiseMode) + ", " + Model.formatBattery(sony.batteryLevel) + ")")
      : "Sony WH-1000XM3 (Disconnected)"

    Row {
      id: contentRow
      anchors.centerIn: parent
      spacing: Style.space(6)

      SonyIcon {
        anchors.verticalCenter: parent.verticalCenter
        iconSize: Style.space(14)
        connected: sony.connected
        batteryLevel: sony.batteryLevel
        charging: sony.batteryCharging
        color: sony.connected
          ? (button.active ? button.activeColor : button.foreground)
          : Qt.rgba(button.foreground.r, button.foreground.g, button.foreground.b, 0.4)
      }

      Text {
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: sony.connected ? Model.formatBattery(sony.batteryLevel) : "—"
        color: button.active ? button.activeColor : button.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        renderType: Text.NativeRendering
        visible: !button.vertical
      }
    }

    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) sony.cycleNoiseMode()
      else root.toggle()
    }
  }

  // ---------------------------------------------------------------------------
  // Dropdown panel
  // ---------------------------------------------------------------------------
  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(400))
    // Tall enough for the longest view (Stable, custom EQ) without scrolling;
    // fittedContentHeight still clamps it to smaller screens.
    contentHeight: panel.fittedContentHeight(panelColumn.implicitHeight, Style.space(1120))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onMoveRequested: function(dx, dy) { root.moveCursor(dx, dy) }
      onActivateRequested: function() { root.activateCursor() }

      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: panelColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
          policy: panelColumn.implicitHeight > panelFlick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }

        Column {
          id: panelColumn
          width: parent.width
          spacing: Style.space(12)

          // -------------------------------------------------------------------
          // Header. An Item rather than a Row: the battery block is pinned to
          // the right edge, and a Row refuses horizontal anchors on its
          // children and then lays out nothing at all.
          // -------------------------------------------------------------------
          Item {
            width: parent.width
            height: Math.max(headerIcon.height, headerText.implicitHeight, batteryBlock.implicitHeight)

            SonyIcon {
              id: headerIcon
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
              iconSize: Style.space(28)
              connected: sony.connected
              batteryLevel: sony.batteryLevel
              charging: sony.batteryCharging
            }

            Column {
              id: headerText
              anchors.left: headerIcon.right
              anchors.leftMargin: Style.space(12)
              anchors.right: batteryBlock.left
              anchors.rightMargin: Style.space(12)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)

              Text {
                width: parent.width
                textFormat: Text.PlainText
                text: sony.deviceName || "WH-1000XM3"
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
                elide: Text.ElideRight
              }

              Row {
                spacing: Style.space(6)

                Rectangle {
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(7)
                  height: Style.space(7)
                  radius: width / 2
                  color: sony.connected ? "#2ecc71" : Qt.darker(root.foreground, 1.8)
                }

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: sony.connected ? (sony.codec ? "Connected • " + sony.codec : "Connected") : "Disconnected"
                  color: root.muted
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                }
              }
            }

            Column {
              id: batteryBlock
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)
              visible: sony.connected

              Row {
                anchors.right: parent.right
                spacing: Style.space(4)

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  text: Model.batteryIcon(sony.batteryLevel, sony.batteryCharging)
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                }

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: Model.formatBattery(sony.batteryLevel)
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                  font.bold: true
                }
              }

              Text {
                anchors.right: parent.right
                textFormat: Text.PlainText
                visible: sony.batteryCharging
                text: "Charging"
                color: "#2ecc71"
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
            }
          }

          ChoiceGrid {
            width: parent.width
            columns: 2
            options: [{ value: "sound", label: "Sound" }, { value: "device", label: "Device" }]
            current: root.currentTab
            cursor: root.cursorIn("tabs")
            foreground: root.foreground
            fontFamily: root.fontFamily
            fontSize: Style.font.bodySmall
            onChosen: function(value) { root.choose("tabs", value) }
          }

          PanelSeparator { foreground: root.foreground }

          // ===================================================================
          // SOUND
          // ===================================================================
          Column {
            width: parent.width
            spacing: Style.space(12)
            visible: root.currentTab === "sound"

            // --- Noise control ------------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              PanelSectionHeader {
                text: sony.optimizerRunning ? "NOISE CONTROL (PAUSED WHILE OPTIMIZING)" : "NOISE CONTROL"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 4
                options: Model.NOISE_MODES.map(function(m) {
                  return { value: m, label: Model.noiseModeButtonLabel(m), icon: Model.noiseModeIcon(m) }
                })
                current: sony.noiseMode
                cursor: root.cursorIn("noise")
                active: !sony.optimizerRunning
                foreground: root.foreground
                fontFamily: root.fontFamily
                fontSize: Style.font.bodySmall
                onChosen: function(value) { root.choose("noise", value) }
              }
            }

            // --- Ambient level and Focus on Voice ------------------------------
            Column {
              width: parent.width
              spacing: Style.space(6)

              HeaderRow {
                width: parent.width
                opacity: sony.noiseMode === Model.NOISE_AMBIENT ? 1.0 : 0.4
                title: "AMBIENT SOUND LEVEL"
                value: sony.noiseMode === Model.NOISE_AMBIENT
                  ? String(Math.round(ambientSlider.dragging ? ambientSlider.liveValue : sony.ambientSoundLevel)) + " / " + sony.ambientMaxLevel
                  : "—"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              PanelSlider {
                id: ambientSlider
                width: parent.width
                opacity: sony.noiseMode === Model.NOISE_AMBIENT ? 1.0 : 0.4
                minimum: Model.STEP_AMBIENT_MIN
                maximum: sony.ambientMaxLevel
                step: 1
                integer: true
                value: Math.max(Model.STEP_AMBIENT_MIN, sony.ambientSoundLevel)
                enabled: sony.noiseMode === Model.NOISE_AMBIENT && !sony.optimizerRunning
                bar: root.bar
                onReleased: function(v) {
                  root.focusSection = "ambient"
                  sony.setAmbientLevel(Math.round(v))
                }
              }

              Toggle {
                width: parent.width
                label: "Focus on Voice"
                description: "Prioritises speech in the ambient passthrough"
                checked: sony.voicePassthrough
                enabled: sony.voiceFocusAvailable
                opacity: sony.voiceFocusAvailable ? 1.0 : 0.5
                hasCursor: root.cursorActive && root.focusSection === "voiceFocus"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: {
                  if (!sony.voiceFocusAvailable) return
                  root.focusSection = "voiceFocus"
                  sony.setVoiceFocus(!sony.voicePassthrough)
                }
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- Bluetooth priority -------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              PanelSectionHeader {
                text: "BLUETOOTH PRIORITY"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 2
                options: [{ value: "quality", label: "Sound quality" }, { value: "stable", label: "Stable" }]
                current: sony.connectionMode
                cursor: root.cursorIn("connection")
                foreground: root.foreground
                fontFamily: root.fontFamily
                fontSize: Style.font.bodySmall
                onChosen: function(value) { root.choose("connection", value) }
              }

              Caption {
                text: sony.dspAvailable
                  ? "EQ, surround and sound position are available. Audio uses SBC instead of LDAC."
                  : "LDAC for the best sound. EQ, surround and sound position need Stable."
                color: root.muted
                font.family: root.fontFamily
              }
            }

            // --- Equalizer (only usable off LDAC) -------------------------------
            PanelSeparator { foreground: root.foreground; visible: sony.dspAvailable }

            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: sony.dspAvailable

              PanelSectionHeader {
                text: "EQUALIZER (" + Model.eqPresetName(sony.eqPreset) + ")"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 4
                options: Model.EQ_PRESETS.map(function(p) {
                  return { value: p, label: Model.eqPresetButtonLabel(p) }
                })
                current: sony.eqPreset
                cursor: root.cursorIn("eq")
                foreground: root.foreground
                fontFamily: root.fontFamily
                onChosen: function(value) { root.choose("eq", value) }
              }

              // Band sliders for Manual, Custom 1 and Custom 2.
              Column {
                width: parent.width
                spacing: Style.space(4)
                visible: sony.customEqSelected

                Repeater {
                  model: 6

                  BandSlider {
                    required property int index
                    width: parent.width
                    label: index < 5 ? Model.EQ_BAND_LABELS[index] + " Hz" : "Clear Bass"
                    value: index < 5 ? (sony.eqCustomBands[index] || 0) : sony.clearBass
                    barRef: root.bar
                    foreground: root.foreground
                    fontFamily: root.fontFamily
                    onCommitted: function(v) {
                      var bands = sony.eqCustomBands.slice()
                      var cb = sony.clearBass
                      if (index < 5) bands[index] = v
                      else cb = v
                      sony.setEqBands(sony.eqPreset, bands, cb)
                    }
                  }
                }
              }
            }

            // --- Surround and sound position (only usable off LDAC) ------------
            PanelSeparator { foreground: root.foreground; visible: sony.dspAvailable }

            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: sony.dspAvailable

              PanelSectionHeader {
                text: "SURROUND (VPT)"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 5
                options: Model.SURROUND_PRESETS.map(function(p) {
                  return { value: p, label: Model.surroundButtonLabel(p) }
                })
                current: sony.surround
                cursor: root.cursorIn("surround")
                foreground: root.foreground
                fontFamily: root.fontFamily
                onChosen: function(value) { root.choose("surround", value) }
              }

              PanelSectionHeader {
                text: "SOUND POSITION"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 3
                options: Model.SOUND_POSITIONS.map(function(p) {
                  return { value: p, label: Model.soundPositionName(p) }
                })
                current: sony.soundPosition
                cursor: root.cursorIn("position")
                foreground: root.foreground
                fontFamily: root.fontFamily
                onChosen: function(value) { root.choose("position", value) }
              }
            }

            PanelSeparator { foreground: root.foreground }

            Toggle {
              width: parent.width
              label: "DSEE HX"
              description: Model.dseeDescription(sony.dseeHx, sony.dseeHxActive, sony.connectionMode)
              checked: sony.dseeHx
              hasCursor: root.cursorActive && root.focusSection === "dsee"
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: {
                root.focusSection = "dsee"
                sony.setDsee(!sony.dseeHx)
              }
            }
          }

          // ===================================================================
          // DEVICE
          // ===================================================================
          Column {
            width: parent.width
            spacing: Style.space(12)
            visible: root.currentTab === "device"

            // --- Headset volume and playback -----------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              HeaderRow {
                width: parent.width
                title: "HEADSET VOLUME"
                value: sony.volume >= 0
                  ? String(Math.round(volumeSlider.dragging ? volumeSlider.liveValue : sony.volume)) + " / " + sony.volumeMax
                  : "—"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              PanelSlider {
                id: volumeSlider
                width: parent.width
                minimum: 0
                maximum: sony.volumeMax
                step: 1
                integer: true
                value: Math.max(0, sony.volume)
                bar: root.bar
                onReleased: function(v) {
                  root.focusSection = "volume"
                  sony.setVolume(Math.round(v))
                }
              }

              ChoiceGrid {
                width: parent.width
                columns: 4
                options: [
                  { value: "previous", label: "Prev", icon: "\u{F04AE}" },
                  { value: "play", label: "Play", icon: "\u{F040A}" },
                  { value: "pause", label: "Pause", icon: "\u{F03E4}" },
                  { value: "next", label: "Next", icon: "\u{F04AD}" }
                ]
                current: ""
                cursor: root.cursorIn("playback")
                foreground: root.foreground
                fontFamily: root.fontFamily
                fontSize: Style.font.bodySmall
                onChosen: function(value) { root.choose("playback", value) }
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- NC Optimizer ---------------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              PanelSectionHeader {
                text: "NC OPTIMIZER"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              Item {
                width: parent.width
                height: Math.max(optimizerText.implicitHeight, optimizerButton.implicitHeight)

                Caption {
                  id: optimizerText
                  anchors.left: parent.left
                  anchors.right: optimizerButton.left
                  anchors.rightMargin: Style.space(12)
                  anchors.verticalCenter: parent.verticalCenter
                  width: undefined
                  text: Model.optimizerDescription(sony.optimizerState, sony.optimizerPressure)
                  color: root.muted
                  font.family: root.fontFamily
                }

                Button {
                  id: optimizerButton
                  anchors.right: parent.right
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(96)
                  text: sony.optimizerRunning ? "Cancel" : "Optimize"
                  fontSize: Style.font.bodySmall
                  foreground: root.foreground
                  fontFamily: root.fontFamily
                  bordered: true
                  selected: sony.optimizerRunning
                  hasCursor: root.cursorActive && root.focusSection === "optimizer"
                  onClicked: {
                    root.focusSection = "optimizer"
                    root.toggleOptimizer()
                  }
                }
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- NC/AMBIENT button ------------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              PanelSectionHeader {
                text: "NC/AMBIENT BUTTON"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 3
                options: Model.NC_BUTTONS.map(function(b) {
                  return { value: b, label: Model.ncButtonName(b) }
                })
                current: sony.ncButton
                cursor: root.cursorIn("ncButton")
                foreground: root.foreground
                fontFamily: root.fontFamily
                onChosen: function(value) { root.choose("ncButton", value) }
              }

              Caption {
                text: Model.ncButtonDescription(sony.ncButton)
                visible: text.length > 0
                color: root.muted
                font.family: root.fontFamily
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- Controls and prompts ---------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              Toggle {
                width: parent.width
                label: "Touch sensor control panel"
                description: "Swipe and tap the right earcup to control playback"
                checked: sony.touchPanel
                hasCursor: root.cursorActive && root.focusSection === "touchPanel"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: {
                  root.focusSection = "touchPanel"
                  sony.setTouchPanel(!sony.touchPanel)
                }
              }

              Toggle {
                width: parent.width
                label: "Voice guidance"
                description: Model.voiceGuidanceDescription(sony.voiceGuidance, sony.voiceGuidanceLanguage)
                checked: sony.voiceGuidance
                hasCursor: root.cursorActive && root.focusSection === "voiceGuidance"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: {
                  root.focusSection = "voiceGuidance"
                  sony.setVoiceGuidance(!sony.voiceGuidance)
                }
              }
            }

            PanelSeparator { foreground: root.foreground }

            // --- Auto power off -----------------------------------------------------
            Column {
              width: parent.width
              spacing: Style.space(8)

              PanelSectionHeader {
                text: "AUTO POWER OFF"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }

              ChoiceGrid {
                width: parent.width
                columns: 5
                options: Model.AUTO_POWER_OFF_VALUES.map(function(v) {
                  return { value: v, label: Model.autoPowerOffLabel(v) }
                })
                current: sony.autoPowerOff
                cursor: root.cursorIn("autoPowerOff")
                foreground: root.foreground
                fontFamily: root.fontFamily
                onChosen: function(value) { root.choose("autoPowerOff", value) }
              }

              Caption {
                text: "Turns the headset off after this long with nothing connected."
                color: root.muted
                font.family: root.fontFamily
              }
            }

            Caption {
              text: (sony.deviceName || "WH-1000XM3") + (sony.firmwareVersion ? " · firmware " + sony.firmwareVersion : "")
              horizontalAlignment: Text.AlignHCenter
              color: root.muted
              font.family: root.fontFamily
            }
          }
        }
      }
    }
  }
}
