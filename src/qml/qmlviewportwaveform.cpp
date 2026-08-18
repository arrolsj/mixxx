#include "qml/qmlviewportwaveform.h"

#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "mixer/basetrackplayer.h"
#include "moc_qmlviewportwaveform.cpp"

namespace {

bool fuzzyEqual(double left, double right) {
    return qFuzzyCompare(left + 1.0, right + 1.0);
}

uchar colorByte(float component) {
    return static_cast<uchar>(
            std::clamp(std::lround(component * 255.0f), 0L, 255L));
}

void addRectangle(QSGGeometry::ColoredPoint2D* pVertices,
        int* pVertexIndex,
        float left,
        float top,
        float right,
        float bottom,
        uchar red,
        uchar green,
        uchar blue,
        uchar alpha) {
    pVertices[(*pVertexIndex)++].set(left, top, red, green, blue, alpha);
    pVertices[(*pVertexIndex)++].set(right, top, red, green, blue, alpha);
    pVertices[(*pVertexIndex)++].set(left, bottom, red, green, blue, alpha);
    pVertices[(*pVertexIndex)++].set(left, bottom, red, green, blue, alpha);
    pVertices[(*pVertexIndex)++].set(right, top, red, green, blue, alpha);
    pVertices[(*pVertexIndex)++].set(right, bottom, red, green, blue, alpha);
}

mixxx::waveform::WaveformRgbColorF toColorF(const QColor& color) {
    return {
            color.redF(),
            color.greenF(),
            color.blueF(),
    };
}

struct PremultipliedColor {
    uchar red;
    uchar green;
    uchar blue;
    uchar alpha;
};

PremultipliedColor toPremultipliedColor(const QColor& color, double opacity) {
    const int alpha = std::clamp(
            static_cast<int>(std::lround(color.alpha() * opacity)), 0, 255);
    return {
            static_cast<uchar>(color.red() * alpha / 255),
            static_cast<uchar>(color.green() * alpha / 255),
            static_cast<uchar>(color.blue() * alpha / 255),
            static_cast<uchar>(alpha),
    };
}

} // namespace

namespace mixxx::qml {

QmlViewportWaveform::QmlViewportWaveform(QQuickItem* parent)
        : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

QmlPlayerProxy* QmlViewportWaveform::player() const {
    return m_pPlayer;
}

void QmlViewportWaveform::setPlayer(QmlPlayerProxy* pPlayer) {
    if (m_pPlayer == pPlayer) {
        return;
    }

    if (m_pPlayer != nullptr) {
        if (BaseTrackPlayer* pTrackPlayer = m_pPlayer->internalTrackPlayer()) {
            pTrackPlayer->disconnect(this);
        }
    }
    setCurrentTrack(nullptr);
    m_pPlayer = pPlayer;

    if (m_pPlayer != nullptr) {
        BaseTrackPlayer* pTrackPlayer = m_pPlayer->internalTrackPlayer();
        if (pTrackPlayer == nullptr) {
            qWarning() << "QmlViewportWaveform received a stale player proxy";
        } else {
            setCurrentTrack(pTrackPlayer->getLoadedTrack());
            refreshTrackEndPositionFromPlayer();
            slotBeatsUpdated();
            connect(pTrackPlayer,
                    &BaseTrackPlayer::newTrackLoaded,
                    this,
                    &QmlViewportWaveform::slotTrackLoaded);
            connect(pTrackPlayer,
                    &BaseTrackPlayer::loadingTrack,
                    this,
                    &QmlViewportWaveform::slotTrackLoading);
            connect(pTrackPlayer,
                    &BaseTrackPlayer::playerEmpty,
                    this,
                    &QmlViewportWaveform::slotTrackUnloaded);
        }
    }

    emit playerChanged();
    updateViewportMetrics();
    update();
}

double QmlViewportWaveform::displayPosition() const {
    return m_displayPosition;
}

void QmlViewportWaveform::setDisplayPosition(double position) {
    if (fuzzyEqual(m_displayPosition, position)) {
        return;
    }
    m_displayPosition = position;
    emit displayPositionChanged();
    if (updateViewportMetrics()) {
        update();
    }
}

double QmlViewportWaveform::zoom() const {
    return m_zoom;
}

void QmlViewportWaveform::setZoom(double zoom) {
    if (fuzzyEqual(m_zoom, zoom)) {
        return;
    }
    m_zoom = zoom;
    emit zoomChanged();
    updateViewportMetrics();
    update();
}

double QmlViewportWaveform::rateRatio() const {
    return m_rateRatio;
}

void QmlViewportWaveform::setRateRatio(double rateRatio) {
    if (fuzzyEqual(m_rateRatio, rateRatio)) {
        return;
    }
    m_rateRatio = rateRatio;
    emit rateRatioChanged();
    updateViewportMetrics();
    update();
}

double QmlViewportWaveform::playMarkerPosition() const {
    return m_playMarkerPosition;
}

void QmlViewportWaveform::setPlayMarkerPosition(double position) {
    if (fuzzyEqual(m_playMarkerPosition, position)) {
        return;
    }
    m_playMarkerPosition = position;
    emit playMarkerPositionChanged();
    updateViewportMetrics();
    update();
}

QColor QmlViewportWaveform::axesColor() const {
    return m_axesColor;
}

void QmlViewportWaveform::setAxesColor(const QColor& color) {
    if (m_axesColor == color) {
        return;
    }
    m_axesColor = color;
    emit axesColorChanged();
    update();
}

QColor QmlViewportWaveform::highColor() const {
    return m_highColor;
}

void QmlViewportWaveform::setHighColor(const QColor& color) {
    if (m_highColor == color) {
        return;
    }
    m_highColor = color;
    emit highColorChanged();
    update();
}

QColor QmlViewportWaveform::midColor() const {
    return m_midColor;
}

void QmlViewportWaveform::setMidColor(const QColor& color) {
    if (m_midColor == color) {
        return;
    }
    m_midColor = color;
    emit midColorChanged();
    update();
}

QColor QmlViewportWaveform::lowColor() const {
    return m_lowColor;
}

void QmlViewportWaveform::setLowColor(const QColor& color) {
    if (m_lowColor == color) {
        return;
    }
    m_lowColor = color;
    emit lowColorChanged();
    update();
}

bool QmlViewportWaveform::beatGridVisible() const {
    return m_beatGridVisible;
}

void QmlViewportWaveform::setBeatGridVisible(bool visible) {
    if (m_beatGridVisible == visible) {
        return;
    }
    m_beatGridVisible = visible;
    emit beatGridVisibleChanged();
    update();
}

bool QmlViewportWaveform::beatGridDownbeatsOnly() const {
    return m_beatGridDownbeatsOnly;
}

void QmlViewportWaveform::setBeatGridDownbeatsOnly(bool downbeatsOnly) {
    if (m_beatGridDownbeatsOnly == downbeatsOnly) {
        return;
    }
    m_beatGridDownbeatsOnly = downbeatsOnly;
    emit beatGridDownbeatsOnlyChanged();
    update();
}

QColor QmlViewportWaveform::beatColor() const {
    return m_beatColor;
}

void QmlViewportWaveform::setBeatColor(const QColor& color) {
    if (m_beatColor == color) {
        return;
    }
    m_beatColor = color;
    emit beatColorChanged();
    update();
}

QColor QmlViewportWaveform::downbeatColor() const {
    return m_downbeatColor;
}

void QmlViewportWaveform::setDownbeatColor(const QColor& color) {
    if (m_downbeatColor == color) {
        return;
    }
    m_downbeatColor = color;
    emit downbeatColorChanged();
    update();
}

double QmlViewportWaveform::trackWidth() const {
    return m_viewportMetrics.trackWidth;
}

double QmlViewportWaveform::contentX() const {
    return m_viewportMetrics.contentX;
}

void QmlViewportWaveform::slotTrackLoaded(TrackPointer pTrack) {
    setCurrentTrack(pTrack);
    refreshTrackEndPositionFromPlayer();
    slotBeatsUpdated();
}

void QmlViewportWaveform::slotTrackLoading(
        TrackPointer pNewTrack, TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    setCurrentTrack(pNewTrack);
}

void QmlViewportWaveform::slotTrackUnloaded() {
    setCurrentTrack(nullptr);
}

void QmlViewportWaveform::setCurrentTrack(TrackPointer pTrack) {
    if (m_pCurrentTrack != pTrack) {
        if (m_pCurrentTrack != nullptr) {
            disconnect(m_pCurrentTrack.get(), nullptr, this, nullptr);
        }
        m_pCurrentTrack = pTrack;
        if (m_pCurrentTrack != nullptr) {
            connect(m_pCurrentTrack.get(),
                    &Track::waveformUpdated,
                    this,
                    &QmlViewportWaveform::slotWaveformUpdated);
            connect(m_pCurrentTrack.get(),
                    &Track::beatsUpdated,
                    this,
                    &QmlViewportWaveform::slotBeatsUpdated);
        }
    }
    m_trackEndPosition = m_pCurrentTrack != nullptr
            ? audio::FramePos{m_pCurrentTrack->getDuration() *
                      m_pCurrentTrack->getSampleRate()}
            : audio::kStartFramePos;
    slotWaveformUpdated();
    slotBeatsUpdated();
}

void QmlViewportWaveform::refreshTrackEndPositionFromPlayer() {
    if (m_pCurrentTrack == nullptr || m_pPlayer == nullptr) {
        return;
    }
    BaseTrackPlayer* pTrackPlayer = m_pPlayer->internalTrackPlayer();
    if (pTrackPlayer == nullptr ||
            pTrackPlayer->getLoadedTrack() != m_pCurrentTrack) {
        return;
    }

    const ConfigKey trackSamplesKey(
            pTrackPlayer->getGroup(), QStringLiteral("track_samples"));
    if (!ControlObject::exists(trackSamplesKey)) {
        return;
    }
    const auto exactTrackEnd = audio::FramePos::fromEngineSamplePosMaybeInvalid(
            ControlObject::get(trackSamplesKey));
    if (exactTrackEnd.isValid() &&
            exactTrackEnd > audio::kStartFramePos) {
        m_trackEndPosition = exactTrackEnd;
    }
}

void QmlViewportWaveform::slotWaveformUpdated() {
    m_pWaveform = m_pCurrentTrack != nullptr
            ? m_pCurrentTrack->getWaveform()
            : ConstWaveformPointer{};
    updateViewportMetrics();
    update();
}

void QmlViewportWaveform::slotBeatsUpdated() {
    if (m_pCurrentTrack != nullptr) {
        m_beatGridData = waveform::buildWaveformBeatGridData(
                m_pCurrentTrack->getBeats(), m_trackEndPosition);
    } else {
        m_beatGridData = {};
        m_trackEndPosition = audio::kStartFramePos;
    }
    update();
}

bool QmlViewportWaveform::updateViewportMetrics() {
    const int dataSize = m_pWaveform != nullptr
            ? m_pWaveform->getDataSize()
            : 0;
    const waveform::WaveformRgbViewportMetrics next =
            waveform::calculateWaveformRgbViewportMetrics(dataSize,
                    width(),
                    m_devicePixelRatio,
                    m_displayPosition,
                    m_playMarkerPosition,
                    m_zoom * m_rateRatio);

    const bool trackWidthHasChanged =
            !fuzzyEqual(m_viewportMetrics.trackWidth, next.trackWidth);
    const bool contentXHasChanged =
            !fuzzyEqual(m_viewportMetrics.contentX, next.contentX);
    const bool renderStateHasChanged =
            m_viewportMetrics.physicalWidth != next.physicalWidth ||
            m_viewportMetrics.visualFrameCount != next.visualFrameCount ||
            m_viewportMetrics.firstGlobalPixel != next.firstGlobalPixel ||
            !fuzzyEqual(m_viewportMetrics.visualFramesPerPhysicalPixel,
                    next.visualFramesPerPhysicalPixel);

    m_viewportMetrics = next;
    if (trackWidthHasChanged) {
        emit trackWidthChanged();
    }
    if (contentXHasChanged) {
        emit contentXChanged();
    }
    return renderStateHasChanged;
}

void QmlViewportWaveform::geometryChange(
        const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    updateViewportMetrics();
    update();
}

void QmlViewportWaveform::itemChange(
        ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged) {
        m_devicePixelRatio = std::max(static_cast<double>(data.realValue), 0.01);
        updateViewportMetrics();
        update();
    } else if (change == ItemSceneChange) {
        m_devicePixelRatio = data.window != nullptr
                ? std::max(static_cast<double>(data.window->devicePixelRatio()), 0.01)
                : 1.0;
        updateViewportMetrics();
        update();
    }
}

QSGNode* QmlViewportWaveform::updatePaintNode(
        QSGNode* pOldNode, UpdatePaintNodeData* pUpdatePaintNodeData) {
    Q_UNUSED(pUpdatePaintNodeData);

    const ConstWaveformPointer pWaveform = m_pWaveform;
    const waveform::WaveformRgbViewportMetrics viewport = m_viewportMetrics;
    if (pWaveform == nullptr || !viewport.isValid() || height() <= 0.0) {
        delete pOldNode;
        return nullptr;
    }

    const int dataSize = pWaveform->getDataSize();
    const int completion = pWaveform->getCompletion();
    const std::vector<waveform::WaveformRgbColumn> columns =
            waveform::sampleWaveformRgbColumns(pWaveform->data(),
                    dataSize,
                    completion,
                    viewport,
                    toColorF(m_lowColor),
                    toColorF(m_midColor),
                    toColorF(m_highColor));

    if (columns.empty()) {
        delete pOldNode;
        return nullptr;
    }

    const std::vector<waveform::WaveformBeatGridLine> beatGridLines =
            m_beatGridVisible
            ? waveform::sampleWaveformBeatGridLines(m_beatGridData,
                      m_trackEndPosition,
                      viewport,
                      m_devicePixelRatio,
                      m_beatGridDownbeatsOnly)
            : std::vector<waveform::WaveformBeatGridLine>{};
    const std::size_t beatGridRectangleCount =
            waveform::waveformBeatGridDashCount(
                    beatGridLines, height(), m_devicePixelRatio);

    auto* pNode = static_cast<QSGGeometryNode*>(pOldNode);
    if (pNode == nullptr) {
        pNode = new QSGGeometryNode();
        auto* pGeometry = new QSGGeometry(
                QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        pGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
        pGeometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        pNode->setGeometry(pGeometry);
        pNode->setFlag(QSGNode::OwnsGeometry);
        auto* pMaterial = new QSGVertexColorMaterial();
        pMaterial->setFlag(QSGMaterial::Blending, true);
        pNode->setMaterial(pMaterial);
        pNode->setFlag(QSGNode::OwnsMaterial);
    }

    QSGGeometry* pGeometry = pNode->geometry();
    constexpr int kVerticesPerRectangle = 6;
    const int vertexCount = kVerticesPerRectangle *
            (static_cast<int>(beatGridRectangleCount) +
                    static_cast<int>(columns.size()) + 1);
    if (pGeometry->vertexCount() != vertexCount) {
        pGeometry->allocate(vertexCount);
    }

    QSGGeometry::ColoredPoint2D* pVertices =
            pGeometry->vertexDataAsColoredPoint2D();
    int vertexIndex = 0;
    const float logicalHeight = static_cast<float>(height());
    const float beatGridStrokeWidth =
            static_cast<float>(2.0 / m_devicePixelRatio);
    for (const auto& line : beatGridLines) {
        const PremultipliedColor color = toPremultipliedColor(
                line.isDownbeat ? m_downbeatColor : m_beatColor,
                line.isDownbeat ? 0.9 : 0.65);
        const float dashLength = beatGridStrokeWidth *
                (line.isDownbeat ? 4.0f : 3.0f);
        const float dashPeriod = dashLength + beatGridStrokeWidth * 3.0f;
        const int dashCount = waveform::waveformBeatGridDashCount(
                height(), m_devicePixelRatio, line.isDownbeat);
        for (int dash = 0; dash < dashCount; ++dash) {
            const float top = dash * dashPeriod;
            addRectangle(pVertices,
                    &vertexIndex,
                    line.x - beatGridStrokeWidth / 2.0f,
                    top,
                    line.x + beatGridStrokeWidth / 2.0f,
                    std::min(top + dashLength, logicalHeight),
                    color.red,
                    color.green,
                    color.blue,
                    color.alpha);
        }
    }

    const float logicalWidth = static_cast<float>(
            viewport.physicalWidth / m_devicePixelRatio);
    const float halfHeight = static_cast<float>(height() / 2.0);
    addRectangle(pVertices,
            &vertexIndex,
            0.0f,
            halfHeight - 0.5f,
            logicalWidth,
            halfHeight + 0.5f,
            static_cast<uchar>(m_axesColor.red()),
            static_cast<uchar>(m_axesColor.green()),
            static_cast<uchar>(m_axesColor.blue()),
            static_cast<uchar>(m_axesColor.alpha()));

    const float amplitudeToHeight = halfHeight / 255.0f * 0.6f;
    const float halfPhysicalPixel =
            static_cast<float>(0.5 / m_devicePixelRatio);
    for (int columnIndex = 0;
            columnIndex < static_cast<int>(columns.size());
            ++columnIndex) {
        const waveform::WaveformRgbColumn& column =
                columns[static_cast<std::size_t>(columnIndex)];
        const float centerX =
                static_cast<float>(columnIndex / m_devicePixelRatio);
        addRectangle(pVertices,
                &vertexIndex,
                centerX - halfPhysicalPixel,
                halfHeight - amplitudeToHeight * column.leftAmplitude,
                centerX + halfPhysicalPixel,
                halfHeight + amplitudeToHeight * column.rightAmplitude,
                colorByte(column.red),
                colorByte(column.green),
                colorByte(column.blue),
                255);
    }

    pGeometry->markVertexDataDirty();
    pNode->markDirty(QSGNode::DirtyGeometry);
    return pNode;
}

} // namespace mixxx::qml
