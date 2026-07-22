import QtQuick 2.15
import QtQuick.Shapes 1.15

Item {
    id: root

    property color color: "#FF0000"
    property real markerWidth: 10
    property real markerHeight: 9

    Shape {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        width: root.markerWidth
        height: root.markerHeight

        ShapePath {
            fillColor: root.color
            strokeColor: "#081012"
            strokeWidth: 1
            startX: 0
            startY: 0
            PathLine { x: root.markerWidth; y: 0 }
            PathLine { x: root.markerWidth / 2; y: root.markerHeight }
            PathLine { x: 0; y: 0 }
        }
    }

    Rectangle {
        x: Math.round((root.width - width) / 2)
        y: root.markerHeight
        width: 1
        height: Math.max(0, root.height - root.markerHeight)
        color: root.color
    }
}
