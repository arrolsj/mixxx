import QtQuick 2.15
import QtQuick.Shapes 1.15

Item {
    id: root

    property color color: "#F8A030"
    property real markerWidth: 14
    property real markerHeight: 11

    Rectangle {
        x: Math.round((root.width - width) / 2)
        y: 0
        width: 1
        height: Math.max(0, root.height - root.markerHeight)
        color: root.color
    }

    Shape {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: root.markerWidth
        height: root.markerHeight

        ShapePath {
            fillColor: root.color
            strokeColor: "#081012"
            strokeWidth: 1
            startX: 0
            startY: root.markerHeight
            PathLine { x: root.markerWidth; y: root.markerHeight }
            PathLine { x: root.markerWidth / 2; y: 0 }
            PathLine { x: 0; y: root.markerHeight }
        }
    }
}
