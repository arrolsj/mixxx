#include "qml/qmlviewportwaveform.h"

#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>

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

constexpr uchar kLowBandAlpha = 230;
constexpr uchar kMidBandAlpha = 218;
constexpr uchar kHighBandAlpha = 242;

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
QmlViewportWaveform::Renderer QmlViewportWaveform::renderer() const {
    return m_renderer;
}

void QmlViewportWaveform::setRenderer(Renderer renderer) {
    if (m_renderer == renderer) {
        return;
    }
    m_renderer = renderer;
    emit rendererChanged();
    update();
}

double QmlViewportWaveform::highGain() const {
    return m_highGain;
}

void QmlViewportWaveform::setHighGain(double gain) {
    const double normalized = std::max(gain, 0.0);
    if (fuzzyEqual(m_highGain, normalized)) {
        return;
    }
    m_highGain = normalized;
    emit highGainChanged();
    update();
}

double QmlViewportWaveform::midGain() const {
    return m_midGain;
}

void QmlViewportWaveform::setMidGain(double gain) {
    const double normalized = std::max(gain, 0.0);
    if (fuzzyEqual(m_midGain, normalized)) {
        return;
    }
    m_midGain = normalized;
    emit midGainChanged();
    update();
}

double QmlViewportWaveform::lowGain() const {
    return m_lowGain;
}

void QmlViewportWaveform::setLowGain(double gain) {
    const double normalized = std::max(gain, 0.0);
    if (fuzzyEqual(m_lowGain, normalized)) {
        return;
    }
    m_lowGain = normalized;
    emit lowGainChanged();
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
    if (m_pCurrentTrack == pTrack) {
        return;
    }
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
                &Track::rekordboxThreeBandWaveformsUpdated,
                this,
                &QmlViewportWaveform::slotWaveformUpdated);
    }
    slotWaveformUpdated();
}

void QmlViewportWaveform::slotWaveformUpdated() {
    m_pWaveform = m_pCurrentTrack != nullptr
            ? m_pCurrentTrack->getWaveform()
            : ConstWaveformPointer{};
    m_pRekordboxThreeBandWaveforms = m_pCurrentTrack != nullptr
            ? m_pCurrentTrack->getRekordboxThreeBandWaveforms()
            : waveform::RekordboxThreeBandWaveformsPointer{};
    m_rekordboxPreviewReference = m_pRekordboxThreeBandWaveforms
            ? waveform::calculateRekordboxThreeBandReference(
                      m_pRekordboxThreeBandWaveforms->preview)
            : waveform::kMixxxThreeBandReference;
    m_rekordboxDetailReference = m_pRekordboxThreeBandWaveforms
            ? waveform::calculateRekordboxThreeBandReference(
                      m_pRekordboxThreeBandWaveforms->detail)
            : waveform::kMixxxThreeBandReference;
    updateViewportMetrics();
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
    std::vector<waveform::WaveformRgbColumn> rgbColumns;
    std::vector<waveform::WaveformThreeBandColumn> threeBandColumns;
    const bool renderThreeBand = m_renderer == Renderer::ThreeBand;
    float threeBandReference = waveform::kMixxxThreeBandReference;
    if (renderThreeBand) {
        const waveform::RekordboxThreeBandWaveformsPointer pRekordbox =
                m_pRekordboxThreeBandWaveforms;
        if (pRekordbox && !pRekordbox->empty()) {
            const bool useDetail = !pRekordbox->detail.empty();
            const auto& source = useDetail
                    ? pRekordbox->detail
                    : pRekordbox->preview;
            threeBandReference = useDetail
                    ? m_rekordboxDetailReference
                    : m_rekordboxPreviewReference;
            threeBandColumns = waveform::sampleRekordboxThreeBandColumns(
                    source, viewport);
        } else {
            threeBandColumns = waveform::sampleWaveformThreeBandColumns(
                    pWaveform->data(), dataSize, completion, viewport);
        }
    } else {
        rgbColumns = waveform::sampleWaveformRgbColumns(pWaveform->data(),
                dataSize,
                completion,
                viewport,
                toColorF(m_lowColor),
                toColorF(m_midColor),
                toColorF(m_highColor));
    }
    const int columnCount = static_cast<int>(renderThreeBand
                    ? threeBandColumns.size()
                    : rgbColumns.size());

    if (columnCount == 0) {
        delete pOldNode;
        return nullptr;
    }

    auto* pNode = static_cast<QSGGeometryNode*>(pOldNode);
    if (pNode == nullptr) {
        pNode = new QSGGeometryNode();
        auto* pGeometry = new QSGGeometry(
                QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        pGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
        pGeometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        pNode->setGeometry(pGeometry);
        pNode->setFlag(QSGNode::OwnsGeometry);
        pNode->setMaterial(new QSGVertexColorMaterial());
        pNode->setFlag(QSGNode::OwnsMaterial);
    }

    QSGGeometry* pGeometry = pNode->geometry();
    constexpr int kVerticesPerRectangle = 6;
    const int vertexCount = kVerticesPerRectangle *
            (columnCount * (renderThreeBand ? 3 : 1) + 1);
    if (pGeometry->vertexCount() != vertexCount) {
        pGeometry->allocate(vertexCount);
    }

    QSGGeometry::ColoredPoint2D* pVertices =
            pGeometry->vertexDataAsColoredPoint2D();
    int vertexIndex = 0;
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

    const float halfPhysicalPixel =
            static_cast<float>(0.5 / m_devicePixelRatio);
    if (!renderThreeBand) {
        const float amplitudeToHeight = halfHeight / 255.0f * 0.6f;
        for (int columnIndex = 0;
                columnIndex < columnCount;
                ++columnIndex) {
            const waveform::WaveformRgbColumn& column =
                    rgbColumns[static_cast<std::size_t>(columnIndex)];
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
    } else {
        for (int columnIndex = 0;
                columnIndex < columnCount;
                ++columnIndex) {
            const waveform::WaveformThreeBandColumn& column =
                    threeBandColumns[static_cast<std::size_t>(columnIndex)];
            const float centerX =
                    static_cast<float>(columnIndex / m_devicePixelRatio);
            const auto appendBand = [&](const QColor& color,
                                            uchar alpha,
                                            float gain,
                                            float leftAmplitude,
                                            float rightAmplitude) {
                addRectangle(pVertices,
                        &vertexIndex,
                        centerX - halfPhysicalPixel,
                        halfHeight - halfHeight *
                                waveform::threeBandDisplayAmplitude(
                                        leftAmplitude, threeBandReference) *
                                gain,
                        centerX + halfPhysicalPixel,
                        halfHeight + halfHeight *
                                waveform::threeBandDisplayAmplitude(
                                        rightAmplitude, threeBandReference) *
                                gain,
                        static_cast<uchar>(color.red()),
                        static_cast<uchar>(color.green()),
                        static_cast<uchar>(color.blue()),
                        alpha);
            };
            // Rekordbox composes all bands on one center axis. Low and mid
            // blend into brown; high is nearly opaque and is painted last.
            appendBand(m_lowColor,
                    kLowBandAlpha,
                    static_cast<float>(m_lowGain),
                    column.leftLow,
                    column.rightLow);
            appendBand(m_midColor,
                    kMidBandAlpha,
                    static_cast<float>(m_midGain),
                    column.leftMid,
                    column.rightMid);
            appendBand(m_highColor,
                    kHighBandAlpha,
                    static_cast<float>(m_highGain),
                    column.leftHigh,
                    column.rightHigh);
        }
    }

    pGeometry->markVertexDataDirty();
    pNode->markDirty(QSGNode::DirtyGeometry);
    return pNode;
}

} // namespace mixxx::qml
