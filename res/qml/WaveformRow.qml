import "." as Skin
import Mixxx 1.0 as Mixxx
import QtQuick 2.14
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
    property string waveformMode: "RGB"

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
            id: zoomControl

            group: root.group
            key: "waveform_zoom"
        }
        Mixxx.ControlProxy {
            id: filterWaveformEnableControl

            group: root.group
            key: "filterWaveformEnable"
        }

        Mixxx.ControlProxy {
            id: filterHighControl

            group: "[EqualizerRack1_" + root.group + "_Effect1]"
            key: "parameter3"
        }

        Mixxx.ControlProxy {
            id: filterHighKillControl

            group: "[EqualizerRack1_" + root.group + "_Effect1]"
            key: "button_parameter3"
        }

        Mixxx.ControlProxy {
            id: filterMidControl

            group: "[EqualizerRack1_" + root.group + "_Effect1]"
            key: "parameter2"
        }

        Mixxx.ControlProxy {
            id: filterMidKillControl

            group: "[EqualizerRack1_" + root.group + "_Effect1]"
            key: "button_parameter2"
        }

        Mixxx.ControlProxy {
            id: filterLowControl

            group: "[EqualizerRack1_" + root.group + "_Effect1]"
            key: "parameter1"
        }

        Mixxx.ControlProxy {
            id: filterLowKillControl

            group: "[EqualizerRack1_" + root.group + "_Effect1]"
            key: "button_parameter1"
        }


        Mixxx.ControlProxy {
            id: introStartPosition

            group: root.group
            key: "intro_start_position"
        }

        Mixxx.ControlProxy {
            id: introEndPosition

            group: root.group
            key: "intro_end_position"
        }

        Mixxx.ControlProxy {
            id: outroStartPosition

            group: root.group
            key: "outro_start_position"
        }

        Mixxx.ControlProxy {
            id: outroEndPosition

            group: root.group
            key: "outro_end_position"
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

        Timer {
            interval: 16
            repeat: true
            running: root.deckPlayer.isLoaded
            onTriggered: {
                const visualPosition = root.deckPlayer.getVisualPlayPosition(interval * 1000);
                waveformContainer.displayedPlayPosition = visualPosition >= 0
                        ? visualPosition
                        : playPositionControl.value;
            }
        }

        // Unlike the former whole-track ShaderEffect, this item always owns
        // exactly one column per physical viewport pixel. Each column is a
        // range maximum on a track-global pixel grid, matching the stable
        // QWidget RGB renderer without creating an unbounded item or texture.
        Mixxx.ViewportWaveform {
            id: viewportWaveform

            anchors.fill: parent
            player: root.deckPlayer
            displayPosition: waveformContainer.displayedPlayPosition
            zoom: zoomControl.value
            rateRatio: rateRatioControl.value
            playMarkerPosition: playMarker.screenPosition
            axesColor: "#FFFFFF"
            renderer: root.waveformMode === "3-BAND"
                    ? Mixxx.ViewportWaveform.Renderer.ThreeBand
                    : Mixxx.ViewportWaveform.Renderer.RGB
            highColor: root.waveformMode === "3-BAND" ? "#F4F7FB" : "#0000FF"
            midColor: root.waveformMode === "3-BAND" ? "#F3A51F" : "#00FF00"
            lowColor: root.waveformMode === "3-BAND" ? "#2F80FF" : "#FF0000"
            highGain: filterWaveformEnableControl.value
                    ? (filterHighKillControl.value ? 0 : filterHighControl.value) : 1
            midGain: filterWaveformEnableControl.value
                    ? (filterMidKillControl.value ? 0 : filterMidControl.value) : 1
            lowGain: filterWaveformEnableControl.value
                    ? (filterLowKillControl.value ? 0 : filterLowControl.value) : 1
            visible: root.deckPlayer.isLoaded
        }

        Item {
            id: waveform

            property real effectiveZoomFactor: waveformContainer.duration > 0
                    ? width / waveformContainer.duration : 0

            width: viewportWaveform.trackWidth
            height: parent.height
            x: viewportWaveform.contentX
            visible: root.deckPlayer.isLoaded

            Repeater {
                model: root.deckPlayer.beatsModel

                Shape {
                    id: beatLine

                    required property real framePosition
                    required property int beatNumber
                    readonly property bool isDownbeat: beatNumber === 1

                    property real physicalPixel: 2 / Screen.devicePixelRatio
                    property real trackX: (beatLine.framePosition * 2 / samplesControl.value) * waveform.width

                    width: physicalPixel
                    height: waveform.height
                    opacity: beatLine.isDownbeat ? 0.9 : 0.65
                    // Snap the final global position, including contentX, so
                    // dashed lines stay crisp while sharing the waveform's
                    // viewport-driven track coordinate transform.
                    x: Math.round((waveform.x + trackX) * Screen.devicePixelRatio) /
                            Screen.devicePixelRatio - waveform.x - width / 2

                    ShapePath {
                        strokeColor: beatLine.isDownbeat ? Theme.waveformDownbeatColor : Theme.waveformBeatColor
                        strokeWidth: beatLine.physicalPixel
                        strokeStyle: ShapePath.DashLine
                        dashPattern: beatLine.isDownbeat ? [4, 3] : [3, 3]
                        fillColor: "transparent"
                        startX: beatLine.width / 2
                        startY: 0

                        PathLine {
                            x: beatLine.width / 2
                            y: beatLine.height
                        }
                    }
                }
            }

            Skin.WaveformIntroOutro {
                id: intro

                visible: introStartPosition.value != -1 || introEndPosition.value != -1

                height: waveform.height
                x: ((introStartPosition.value != -1 ? introStartPosition.value : introEndPosition.value) / samplesControl.value) * waveform.width
                width: introEndPosition.value == -1 ? 0 : ((introEndPosition.value - introStartPosition.value) / samplesControl.value) * waveform.width
            }

            Skin.WaveformIntroOutro {
                id: outro

                visible: outroStartPosition.value != -1 || outroEndPosition.value != -1
                isIntro: false

                height: waveform.height
                x: ((outroStartPosition.value != -1 ? outroStartPosition.value : outroEndPosition.value) / samplesControl.value) * waveform.width
                width: outroEndPosition.value == -1 || outroStartPosition.value == -1 ? 0 : ((outroEndPosition.value - outroStartPosition.value) / samplesControl.value) * waveform.width
            }

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

                    Skin.WaveformHotcue {
                        group: root.group
                        hotcueNumber: cue.hotcueNumber + 1
                        label: cue.label
                        isLoop: cue.isLoop

                        x: (startPosition * 2 / samplesControl.value) * waveform.width
                        width: cue.isLoop ? ((endPosition - startPosition) * 2 / samplesControl.value) * waveform.width : null
                        height: waveform.height
                    }
                }
            }

            Skin.WaveformCue {
                id: maincue

                height: waveform.height
                x: (mainCuePosition.value / samplesControl.value) * waveform.width
            }
        }
    }

    Shape {
        id: playMarkerShape

        anchors.fill: parent

        ShapePath {
            id: playMarker

            property real screenPosition: 0.5

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
