import QtQuick 2.15

Item {
    id: root

    property color color: "#FF0000"
    property real markerWidth: 12
    property real markerHeight: 11

    Canvas {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        width: root.markerWidth
        height: root.markerHeight
        antialiasing: true

        onPaint: {
            const context = getContext("2d");
            context.clearRect(0, 0, width, height);
            context.fillStyle = root.color;
            context.beginPath();
            context.moveTo(0, 0);
            context.lineTo(width, 0);
            context.lineTo(width / 2, height);
            context.closePath();
            context.fill();
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
