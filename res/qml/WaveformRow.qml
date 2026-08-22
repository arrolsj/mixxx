import "." as Skin
import Mixxx 1.0 as Mixxx
import QtQuick 6.4
import QtQuick.Shapes 1.12
import QtQuick.Window 2.14
import "Theme"

Item {
    id: root

    enum MouseStatus {
        Normal,
        Bending,
        Scratching
    }

    property string group // required
    property var deckPlayer: Mixxx.PlayerManager.getPlayer(group)
    property color memoryCueColor: "#FF0000"
    property color loopCueColor: "#F8A030"
    property color mainCueColor: "#F8A030"
    property real waveformZoom: zoomControl.value
    property real playMarkerPosition: 0.5
    property bool beatGridDownbeatsOnly: false
    property string waveformMode: "RGB"
    readonly property bool visualPositionUpdateRequired: deckPlayer.isLoaded
    readonly property bool waveformRefreshRequired: visualPositionUpdateRequired
    property real previousDiagnosticPosition: NaN
    property real previousDiagnosticElapsedSeconds: -1
    property real stagnantDiagnosticStartSeconds: -1
    property bool positionStallCandidateLogged: false
    property real pixelWindowStartSeconds: -1
    property real pixelWindowStartX: NaN
    property bool pixelStallCandidateLogged: false

    function resetPositionStallDiagnostics() {
        previousDiagnosticPosition = NaN;
        previousDiagnosticElapsedSeconds = -1;
        stagnantDiagnosticStartSeconds = -1;
        positionStallCandidateLogged = false;
        pixelWindowStartSeconds = -1;
        pixelWindowStartX = NaN;
        pixelStallCandidateLogged = false;
    }

    function observeWaveformPixel(position, monotonicElapsedSeconds) {
        const contentX = viewportWaveform.contentX;
        if (!isFinite(contentX) || pixelWindowStartSeconds < 0 ||
                !isFinite(pixelWindowStartX)) {
            pixelWindowStartSeconds = monotonicElapsedSeconds;
            pixelWindowStartX = contentX;
            pixelStallCandidateLogged = false;
            return;
        }

        const windowSeconds = monotonicElapsedSeconds - pixelWindowStartSeconds;
        if (windowSeconds < 0.03)
            return;

        // The viewport's rateRatio scaling keeps nominal scroll speed at
        // 441 visual frames/second divided by zoom, independent of pitch.
        const expectedPixels = windowSeconds * 441 / root.waveformZoom;
        const actualPixels = Math.abs(contentX - pixelWindowStartX);
        if (expectedPixels > 0 && actualPixels < expectedPixels * 0.25) {
            if (!pixelStallCandidateLogged) {
                pixelStallCandidateLogged = true;
                console.info("LASTCUE_STALL_WAVEFORM_PIXEL",
                        "group=" + root.group,
                        "window_ms=" + (windowSeconds * 1000).toFixed(3),
                        "monotonic_s=" + monotonicElapsedSeconds.toFixed(6),
                        "expected_px=" + expectedPixels.toFixed(3),
                        "actual_px=" + actualPixels.toFixed(3),
                        "content_x=" + contentX.toFixed(3),
                        "position=" + position.toPrecision(17),
                        "published=" + playPositionControl.value.toPrecision(17));
            }
            return;
        }

        if (pixelStallCandidateLogged) {
            console.info("LASTCUE_STALL_WAVEFORM_PIXEL_RECOVERY",
                    "group=" + root.group,
                    "duration_ms=" + (windowSeconds * 1000).toFixed(3),
                    "monotonic_s=" + monotonicElapsedSeconds.toFixed(6),
                    "expected_px=" + expectedPixels.toFixed(3),
                    "actual_px=" + actualPixels.toFixed(3),
                    "content_x=" + contentX.toFixed(3),
                    "position=" + position.toPrecision(17));
        }
        pixelWindowStartSeconds = monotonicElapsedSeconds;
        pixelWindowStartX = contentX;
        pixelStallCandidateLogged = false;
    }

    function observeVisualPosition(position, monotonicElapsedSeconds) {
        if (!isFinite(position) || !isFinite(monotonicElapsedSeconds) ||
                monotonicElapsedSeconds < 0 || playControl.value <= 0 ||
                Math.abs(rateRatioControl.value) < 0.000001) {
            resetPositionStallDiagnostics();
            return;
        }

        const positionMoved = !isFinite(previousDiagnosticPosition) ||
                Math.abs(position - previousDiagnosticPosition) > 0.000000000001;
        if (positionMoved) {
            if (positionStallCandidateLogged) {
                const stagnantDurationMs = Math.max(0,
                        (monotonicElapsedSeconds -
                        stagnantDiagnosticStartSeconds) * 1000);
                console.info("LASTCUE_STALL_VISUAL_POSITION_RECOVERY",
                        "group=" + root.group,
                        "duration_ms=" + stagnantDurationMs.toFixed(3),
                        "monotonic_s=" + monotonicElapsedSeconds.toFixed(6),
                        "previous=" + previousDiagnosticPosition.toPrecision(17),
                        "current=" + position.toPrecision(17),
                        "published=" + playPositionControl.value.toPrecision(17));
            }
            stagnantDiagnosticStartSeconds = -1;
            positionStallCandidateLogged = false;
        } else {
            if (stagnantDiagnosticStartSeconds < 0)
                stagnantDiagnosticStartSeconds = previousDiagnosticElapsedSeconds;
            const stagnantDurationMs = Math.max(0,
                    (monotonicElapsedSeconds -
                    stagnantDiagnosticStartSeconds) * 1000);
            if (!positionStallCandidateLogged && stagnantDurationMs > 30) {
                positionStallCandidateLogged = true;
                console.info("LASTCUE_STALL_VISUAL_POSITION",
                        "group=" + root.group,
                        "duration_ms=" + stagnantDurationMs.toFixed(3),
                        "threshold_ms=30",
                        "monotonic_s=" + monotonicElapsedSeconds.toFixed(6),
                        "position=" + position.toPrecision(17),
                        "published=" + playPositionControl.value.toPrecision(17));
            }
        }

        previousDiagnosticPosition = position;
        previousDiagnosticElapsedSeconds = monotonicElapsedSeconds;
        observeWaveformPixel(position, monotonicElapsedSeconds);
    }

    function refreshVisualPosition(monotonicElapsedSeconds) {
        if (!visualPositionUpdateRequired) {
            resetPositionStallDiagnostics();
            return;
        }
        const visualPosition = deckPlayer.getVisualPlayPosition(
                waveformContainer.displayFrameIntervalMicros);
        const displayedPosition = visualPosition >= 0
                ? visualPosition
                : playPositionControl.value;
        waveformContainer.displayedPlayPosition = displayedPosition;
        observeVisualPosition(displayedPosition, monotonicElapsedSeconds);
    }

    function refreshWaveform(monotonicElapsedSeconds) {
        refreshVisualPosition(monotonicElapsedSeconds);
    }

    Item {
        id: waveformContainer

        property real duration: samplesControl.value / (2 * sampleRateControl.value)

        anchors.fill: parent
        clip: true

        Mixxx.ControlProxy {
            id: samplesControl

            group: root.group
            key: "track_samples"
        }

        Mixxx.ControlProxy {
            id: sampleRateControl

            group: root.group
            key: "track_samplerate"
        }

        Mixxx.ControlProxy {
            id: playPositionControl

            group: root.group
            key: "playposition"
        }

        Mixxx.ControlProxy {
            id: rateRatioControl

            group: root.group
            key: "rate_ratio"
        }

        Mixxx.ControlProxy {
            id: playControl

            group: root.group
            key: "play"
        }

        Mixxx.ControlProxy {
            id: zoomControl

            group: root.group
            key: "waveform_zoom"
        }

        Mixxx.ControlProxy {
            id: loopStartPosition

            group: root.group
            key: "loop_start_position"
        }

        Mixxx.ControlProxy {
            id: loopEndPosition

            group: root.group
            key: "loop_end_position"
        }

        Mixxx.ControlProxy {
            id: loopEnabled

            group: root.group
            key: "loop_enabled"
        }

        Mixxx.ControlProxy {
            id: mainCuePosition

            group: root.group
            key: "cue_point"
        }

        // The generic playposition control is intentionally published at only
        // 15 Hz. Sample the audio engine's VisualPlayPosition at display cadence
        // instead. This is the same clock, DAC compensation, rate and loop-boundary
        // calculation used by the stable QWidget waveform; no QML-side velocity
        // estimation or correction is applied.
        property real displayedPlayPosition: playPositionControl.value
        readonly property int displayFrameIntervalMicros: 17134

        // Unlike the former whole-track ShaderEffect, this item always owns
        // exactly one column per physical viewport pixel. Each column is a
        // range maximum on a track-global pixel grid, matching the stable
        // QWidget RGB renderer without creating an unbounded item or texture.
        // The beat grid is batched into the same bounded QSG geometry so zoom
        // never fans out across independent QML Shape delegates.
        Mixxx.ViewportWaveform {
            id: viewportWaveform

            anchors.fill: parent
            player: root.deckPlayer
            displayPosition: waveformContainer.displayedPlayPosition
            zoom: root.waveformZoom
            rateRatio: rateRatioControl.value
            playMarkerPosition: root.playMarkerPosition
            axesColor: "#FFFFFF"
            highColor: "#0000FF"
            midColor: "#00FF00"
            lowColor: "#FF0000"
            renderer: root.waveformMode === "3-BAND"
                    ? Mixxx.ViewportWaveform.Renderer.ThreeBand
                    : Mixxx.ViewportWaveform.Renderer.RGB
            beatGridVisible: true
            beatGridDownbeatsOnly: root.beatGridDownbeatsOnly
            beatColor: Theme.waveformBeatColor
            downbeatColor: Theme.waveformDownbeatColor
            visible: root.deckPlayer.isLoaded
            z: 1
        }

        Item {
            id: waveform

            property real effectiveZoomFactor: waveformContainer.duration > 0
                    ? width / waveformContainer.duration : 0

            width: viewportWaveform.trackWidth
            height: parent.height
            x: viewportWaveform.contentX
            visible: root.deckPlayer.isLoaded
            z: 2

            Skin.WaveformLoop {
                id: loop

                visible: loopStartPosition.value != -1 && loopEndPosition.value != -1

                height: waveform.height
                x: (loopStartPosition.value / samplesControl.value) * waveform.width
                width: ((loopEndPosition.value - loopStartPosition.value) / samplesControl.value) * waveform.width
                enabled: loopEnabled.value
            }

            Repeater {
                model: root.deckPlayer.hotcuesModel

                Item {
                    id: cue

                    required property int startPosition
                    required property int endPosition
                    required property string label
                    required property bool isLoop
                    required property int hotcueNumber
                    required property color color
                    z: cue.isLoop ? 2 : 1

                    Skin.WaveformHotcue {
                        group: root.group
                        hotcueNumber: cue.hotcueNumber + 1
                        label: cue.label
                        isLoop: cue.isLoop
                        cueColor: cue.color
                        loopColor: root.loopCueColor

                        x: (startPosition * 2 / samplesControl.value) * waveform.width
                        width: cue.isLoop
                                ? ((endPosition - startPosition) * 2 / samplesControl.value) * waveform.width
                                : markerSize
                        height: waveform.height
                    }
                }
            }

            Repeater {
                model: root.deckPlayer.memoryCuesModel

                Item {
                    id: memoryCue

                    required property int startPosition

                    x: (startPosition * 2 / samplesControl.value) * waveform.width - width / 2
                    width: 12
                    height: waveform.height
                    z: 3

                    Skin.WaveformMemoryCue {
                        anchors.fill: parent
                        color: root.memoryCueColor
                    }
                }
            }

            Skin.WaveformCue {
                id: maincue

                visible: mainCuePosition.value >= 0
                height: waveform.height
                width: 10
                x: (mainCuePosition.value / samplesControl.value) * waveform.width - width / 2
                color: root.mainCueColor
                z: 4
            }
        }
    }

    Shape {
        id: playMarkerShape

        anchors.fill: parent

        ShapePath {
            id: playMarker

            property real screenPosition: root.playMarkerPosition

            startX: playMarkerShape.width * playMarker.screenPosition
            startY: 0
            strokeColor: Theme.waveformCursorColor
            strokeWidth: 1

            PathLine {
                id: marker

                x: playMarkerShape.width * playMarker.screenPosition
                y: playMarkerShape.height
            }
        }
    }

    Mixxx.ControlProxy {
        id: scratchPositionEnableControl

        group: root.group
        key: "scratch_position_enable"
    }

    Mixxx.ControlProxy {
        id: scratchPositionControl

        group: root.group
        key: "scratch_position"
    }

    Mixxx.ControlProxy {
        id: wheelControl

        group: root.group
        key: "wheel"
    }

    MouseArea {
        property int mouseStatus: WaveformRow.MouseStatus.Normal
        property point mouseAnchor: Qt.point(0, 0)

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: {
            mouseAnchor = Qt.point(mouse.x, mouse.y);
            if (mouse.button == Qt.LeftButton) {
                if (mouseStatus == WaveformRow.MouseStatus.Bending)
                    wheelControl.parameter = 0.5;

                mouseStatus = WaveformRow.MouseStatus.Scratching;
                scratchPositionEnableControl.value = 1;
                // TODO: Calculate position properly
                scratchPositionControl.value = -mouse.x * waveform.effectiveZoomFactor * 2;
                console.log(mouse.x);
            } else {
                if (mouseStatus == WaveformRow.MouseStatus.Scratching)
                    scratchPositionEnableControl.value = 0;

                wheelControl.parameter = 0.5;
                mouseStatus = WaveformRow.MouseStatus.Bending;
            }
        }
        onPositionChanged: {
            switch (mouseStatus) {
                case WaveformRow.MouseStatus.Bending: {
                    const diff = mouse.x - mouseAnchor.x;
                    // Start at the middle of [0.0, 1.0], and emit values based on how far
                    // the mouse has traveled horizontally. Note, for legacy (MIDI) reasons,
                    // this is tuned to 127.
                    const v = 0.5 + (diff / 1270);
                    // clamp to [0.0, 1.0]
                    wheelControl.parameter = Mixxx.MathUtils.clamp(v, 0, 1);
                    break;
                };
                case WaveformRow.MouseStatus.Scratching:
                // TODO: Calculate position properly
                    scratchPositionControl.value = -mouse.x * waveform.effectiveZoomFactor * 2;
                    break;
            }
        }
        onReleased: {
            switch (mouseStatus) {
                case WaveformRow.MouseStatus.Bending:
                    wheelControl.parameter = 0.5;
                    break;
                case WaveformRow.MouseStatus.Scratching:
                    scratchPositionEnableControl.value = 0;
                    break;
            }
            mouseStatus = WaveformRow.MouseStatus.Normal;
        }
    }
}
