import "." as Skin
import QtQuick 2.15

Item {
    id: root

    required property int hotcueNumber
    required property string group
    required property string label
    required property color cueColor
    property bool isLoop: false
    property color loopColor: "#F8A030"
    property real markerSize: 14

    readonly property color markerColor: root.isLoop
            ? root.loopColor
            : (root.cueColor.a > 0 ? root.cueColor : hotcue.color)
    readonly property string hotcueLetter: root.hotcueNumber >= 1 && root.hotcueNumber <= 8
            ? String.fromCharCode(64 + root.hotcueNumber) : ""

    Skin.Hotcue {
        id: hotcue

        group: root.group
        hotcueNumber: root.hotcueNumber
    }

    Rectangle {
        x: 0
        y: root.markerSize
        width: root.isLoop ? root.width : 1
        height: Math.max(0, root.height - root.markerSize)
        visible: root.isLoop && root.width > 0
        color: Qt.rgba(root.markerColor.r,
                root.markerColor.g, root.markerColor.b, 0.14)
        border.width: 1
        border.color: root.markerColor
    }

    Rectangle {
        x: 0
        y: root.markerSize
        width: 1
        height: Math.max(0, root.height - root.markerSize)
        color: root.markerColor
    }

    Rectangle {
        x: 0
        y: 0
        width: root.markerSize
        height: root.markerSize
        color: root.markerColor

        Text {
            anchors.centerIn: parent
            color: "#071012"
            font.pixelSize: 9
            font.weight: Font.Bold
            text: root.hotcueLetter
        }
    }
}
