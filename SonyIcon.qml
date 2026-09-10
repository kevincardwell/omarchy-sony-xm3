// plugin/SonyIcon.qml
// Vector headphone silhouette with dynamic state/alert coloring.
import QtQuick
import QtQuick.Shapes
import qs.Commons

Item {
  id: root

  property real iconSize: Style.font.icon
  property bool connected: true
  property int batteryLevel: -1
  property bool charging: false

  property color normalColor: Color.foreground
  property color alertColor: Color.urgent
  property color chargingColor: "#2ecc71"
  property color disconnectedColor: Qt.rgba(normalColor.r, normalColor.g, normalColor.b, 0.35)

  readonly property color effectiveColor: {
    if (!connected) return disconnectedColor
    if (charging) return chargingColor
    if (batteryLevel >= 0 && batteryLevel < 20) return alertColor
    return normalColor
  }

  property color color: effectiveColor

  width: iconSize
  height: iconSize
  implicitWidth: iconSize
  implicitHeight: iconSize

  Behavior on color {
    ColorAnimation { duration: 180 }
  }

  // Headband arch
  Shape {
    anchors.fill: parent
    antialiasing: true
    layer.enabled: true
    layer.samples: 4

    ShapePath {
      strokeWidth: Math.max(1.8, root.width * 0.115)
      strokeColor: root.color
      fillColor: "transparent"
      capStyle: ShapePath.RoundCap
      joinStyle: ShapePath.RoundJoin
      startX: root.width * 0.18
      startY: root.height * 0.55

      PathCubic {
        control1X: root.width * 0.18
        control1Y: root.height * 0.05
        control2X: root.width * 0.82
        control2Y: root.height * 0.05
        x: root.width * 0.82
        y: root.height * 0.55
      }
    }
  }

  // Left Ear Cup
  Rectangle {
    x: root.width * 0.06
    y: root.height * 0.44
    width: root.width * 0.24
    height: root.height * 0.48
    radius: width * 0.48
    color: root.color
    antialiasing: true

    Behavior on color {
      ColorAnimation { duration: 180 }
    }
  }

  // Right Ear Cup
  Rectangle {
    x: root.width * 0.70
    y: root.height * 0.44
    width: root.width * 0.24
    height: root.height * 0.48
    radius: width * 0.48
    color: root.color
    antialiasing: true

    Behavior on color {
      ColorAnimation { duration: 180 }
    }
  }
}
