#include "qml/qmlwaveformoverview.h"

#include <algorithm>
#include <cmath>

#include "mixer/basetrackplayer.h"
#include "moc_qmlwaveformoverview.cpp"
#include "qml/qmlwaveformpalette.h"
#include "waveform/renderers/waveformrgbcolumnsampler.h"

namespace {
constexpr double kDesiredChannelHeight = 255;
constexpr int kProgressRefreshIntervalMillis = 67;
} // namespace

namespace mixxx {
namespace qml {

QmlWaveformOverview::QmlWaveformOverview(QQuickItem* parent)
        : QQuickPaintedItem(parent),
          m_pPlayer(nullptr),
          m_channels(ChannelFlag::BothChannels),
          m_renderer(Renderer::RGB),
          m_colorHigh(0xFF0000),
          m_colorMid(0x00FF00),
          m_colorLow(0x0000FF),
          m_lastProgressCompletion(-1),
          m_paintedProgressCompletion(0) {
    m_progressTimer.setInterval(kProgressRefreshIntervalMillis);
    m_progressTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_progressTimer,
            &QTimer::timeout,
            this,
            &QmlWaveformOverview::slotWaveformProgressTimer);
}

QmlPlayerProxy* QmlWaveformOverview::getPlayer() const {
    return m_pPlayer;
}

void QmlWaveformOverview::setPlayer(QmlPlayerProxy* pPlayer) {
    if (m_pPlayer == pPlayer) {
        return;
    }

    if (m_pPlayer != nullptr) {
        m_pPlayer->internalTrackPlayer()->disconnect(this);
    }

    m_pPlayer = pPlayer;

    if (m_pPlayer != nullptr) {
        BaseTrackPlayer* pTrackPlayer = m_pPlayer->internalTrackPlayer();
        if (pTrackPlayer == nullptr) {
            qWarning() << "QmlWaveformOverview received a stale player proxy";
            emit playerChanged();
            invalidatePaintedProgress();
            return;
        }
        setCurrentTrack(pTrackPlayer->getLoadedTrack());
        connect(pTrackPlayer,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                &QmlWaveformOverview::slotTrackLoaded);
        connect(pTrackPlayer,
                &BaseTrackPlayer::loadingTrack,
                this,
                &QmlWaveformOverview::slotTrackLoading);
        connect(pTrackPlayer,
                &BaseTrackPlayer::playerEmpty,
                this,
                &QmlWaveformOverview::slotTrackUnloaded);
    }

    emit playerChanged();
    invalidatePaintedProgress();
}

QmlWaveformOverview::Channels QmlWaveformOverview::getChannels() const {
    return m_channels;
}

void QmlWaveformOverview::setChannels(QmlWaveformOverview::Channels channels) {
    if (m_channels == channels) {
        return;
    }

    m_channels = channels;
    emit channelsChanged(channels);
    invalidatePaintedProgress();
}

QmlWaveformOverview::Renderer QmlWaveformOverview::getRenderer() const {
    return m_renderer;
}

void QmlWaveformOverview::setRenderer(Renderer renderer) {
    if (m_renderer == renderer) {
        return;
    }
    m_renderer = renderer;
    emit rendererChanged(renderer);
    invalidatePaintedProgress();
}

QVariantMap QmlWaveformOverview::getThreeBandColors() const {
    return m_threeBandColors;
}

void QmlWaveformOverview::setThreeBandColors(const QVariantMap& colors) {
    if (m_threeBandColors == colors) {
        return;
    }
    m_threeBandColors = colors;
    m_threeBandPalette = waveformLayerPaletteFromMap(colors);
    emit threeBandColorsChanged();
    invalidatePaintedProgress();
}

void QmlWaveformOverview::geometryChange(
        const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        invalidatePaintedProgress();
    }
}

void QmlWaveformOverview::slotTrackLoaded(TrackPointer pTrack) {
    // TODO: Investigate if it's a bug that this debug assertion fails when
    // passing tracks on the command line
    // DEBUG_ASSERT(m_pCurrentTrack == pTrack);
    setCurrentTrack(pTrack);
}

void QmlWaveformOverview::slotTrackLoading(TrackPointer pNewTrack, TrackPointer pOldTrack) {
    DEBUG_ASSERT(m_pCurrentTrack == pOldTrack);
    setCurrentTrack(pNewTrack);
}

void QmlWaveformOverview::slotTrackUnloaded() {
    setCurrentTrack(nullptr);
}

void QmlWaveformOverview::setCurrentTrack(TrackPointer pTrack) {
    // TODO: Check if this is actually possible
    if (m_pCurrentTrack == pTrack) {
        return;
    }

    if (m_pCurrentTrack != nullptr) {
        disconnect(m_pCurrentTrack.get(), nullptr, this, nullptr);
    }

    m_pCurrentTrack = pTrack;
    m_pProgressWaveform.reset();
    if (pTrack != nullptr) {
        connect(pTrack.get(),
                &Track::waveformSummaryUpdated,
                this,
                &QmlWaveformOverview::slotWaveformUpdated);
    }
    slotWaveformUpdated();
}

void QmlWaveformOverview::slotWaveformUpdated() {
    const TrackPointer pTrack = m_pCurrentTrack;
    const ConstWaveformPointer pWaveform =
            pTrack ? pTrack->getWaveformSummary() : ConstWaveformPointer{};
    const bool isExistingProgressWaveform =
            pWaveform && pWaveform == m_pProgressWaveform;
    m_pProgressWaveform = pWaveform;
    m_lastProgressCompletion = pWaveform ? pWaveform->getCompletion() : -1;
    updateProgressTimer();
    if (isExistingProgressWaveform) {
        updateProgress(m_lastProgressCompletion, pWaveform->getDataSize());
    } else {
        invalidatePaintedProgress();
    }
}

void QmlWaveformOverview::slotWaveformProgressTimer() {
    const TrackPointer pTrack = m_pCurrentTrack;
    const ConstWaveformPointer pWaveform =
            pTrack ? pTrack->getWaveformSummary() : ConstWaveformPointer{};
    if (!pWaveform) {
        m_progressTimer.stop();
        return;
    }

    const int completion = pWaveform->getCompletion();
    if (completion != m_lastProgressCompletion) {
        m_lastProgressCompletion = completion;
        updateProgress(completion, pWaveform->getDataSize());
    }
    if (completion >= pWaveform->getDataSize()) {
        m_progressTimer.stop();
    }
}

void QmlWaveformOverview::invalidatePaintedProgress() {
    m_paintedProgressCompletion = 0;
    update();
}

void QmlWaveformOverview::updateProgress(int completion, int dataSize) {
    if (completion < m_paintedProgressCompletion) {
        invalidatePaintedProgress();
        return;
    }
    if (completion == m_paintedProgressCompletion) {
        return;
    }

    // Keep a pending full repaint from being narrowed to a partial update.
    if (m_paintedProgressCompletion == 0 || dataSize <= 0 ||
            width() <= 0 || height() <= 0) {
        update();
        return;
    }

    const qreal startX =
            m_paintedProgressCompletion * width() / dataSize;
    const qreal endX = completion * width() / dataSize;
    const QRect dirtyRect =
            QRectF(startX, 0, endX - startX, height()).toAlignedRect();
    update(dirtyRect);
}

void QmlWaveformOverview::updateProgressTimer() {
    const TrackPointer pTrack = m_pCurrentTrack;
    const ConstWaveformPointer pWaveform =
            pTrack ? pTrack->getWaveformSummary() : ConstWaveformPointer{};
    if (!pWaveform || pWaveform->getDataSize() <= 0 ||
            pWaveform->getCompletion() >= pWaveform->getDataSize()) {
        m_progressTimer.stop();
        return;
    }
    if (!m_progressTimer.isActive()) {
        m_progressTimer.start();
    }
}

void QmlWaveformOverview::paint(QPainter* pPainter) {
    if (!pPainter->hasClipping()) {
        m_paintedProgressCompletion = 0;
    }

    TrackPointer pTrack = m_pCurrentTrack;
    if (!pTrack) {
        return;
    }

    ConstWaveformPointer pWaveform = pTrack->getWaveformSummary();
    if (!pWaveform) {
        return;
    }

    const int dataSize = pWaveform->getDataSize();
    if (dataSize == 0) {
        return;
    }

    int actualCompletion = 0;
    if (pPainter->hasClipping() && width() > 0) {
        const int clippedCompletion = static_cast<int>(std::floor(
                pPainter->clipBoundingRect().left() * dataSize / width()));
        actualCompletion = std::min(m_paintedProgressCompletion,
                std::max(0, clippedCompletion / 2 * 2));
    }
    // Always multiple of 2
    const int waveformCompletion = m_lastProgressCompletion >= 0
            ? std::min(pWaveform->getCompletion(), m_lastProgressCompletion)
            : pWaveform->getCompletion();
    // Test if there is some new to draw (at least of pixel width)
    const int completionIncrement = waveformCompletion - actualCompletion;

    const qreal desiredWidth = static_cast<qreal>(dataSize) / 2;
    const double visiblePixelIncrement = completionIncrement * desiredWidth / dataSize;
    if (waveformCompletion < (dataSize - 2) &&
            (completionIncrement < 2 || visiblePixelIncrement == 0)) {
        return;
    }

    const int nextCompletion = actualCompletion + completionIncrement;

    const Channels channels = m_channels;
    pPainter->save();

    switch (channels) {
    case static_cast<int>(ChannelFlag::LeftChannel):
        // Draw both channels.
        // Set the y axis to half the height of the item
        pPainter->translate(0.0, height());
        // Set the x axis to half the height of the item
        pPainter->scale(width() / desiredWidth, height() / kDesiredChannelHeight);
        break;
    case static_cast<int>(ChannelFlag::RightChannel):
        // Set the x axis to half the height of the item
        pPainter->scale(width() / desiredWidth, height() / kDesiredChannelHeight);
        break;
    default:
        // Draw both channels.
        // Set the y axis to half the height of the item
        pPainter->translate(0.0, height() / 2);
        // Set the x axis to half the height of the item
        pPainter->scale(width() / desiredWidth, height() / (2 * kDesiredChannelHeight));
    }

    Renderer renderer = m_renderer;
    for (int currentCompletion = actualCompletion;
            currentCompletion < nextCompletion;
            currentCompletion += 2) {
        switch (renderer) {
        case Renderer::Filtered:
            drawFiltered(pPainter, channels, pWaveform, currentCompletion);
            break;
        default:
            drawRgb(pPainter, channels, pWaveform, currentCompletion);
        }
    }
    pPainter->restore();
    m_paintedProgressCompletion = nextCompletion;
}

void QmlWaveformOverview::drawRgb(QPainter* pPainter,
        Channels channels,
        ConstWaveformPointer pWaveform,
        int completion) const {
    const double offsetX = completion / 2.0;

    if (channels.testFlag(ChannelFlag::LeftChannel)) {
        // Draw left channel
        const QColor leftColor = getRgbPenColor(pWaveform, completion);
        if (leftColor.isValid()) {
            const uint8_t leftValue = pWaveform->getAll(completion);
            pPainter->setPen(leftColor);
            pPainter->drawLine(QPointF(offsetX, -leftValue), QPointF(offsetX, 0.0));
        }
    }

    if (channels.testFlag(ChannelFlag::RightChannel)) {
        // Draw right channel
        QColor rightColor = getRgbPenColor(pWaveform, completion + 1);
        if (rightColor.isValid()) {
            const uint8_t rightValue = pWaveform->getAll(completion + 1);
            pPainter->setPen(rightColor);
            pPainter->drawLine(QPointF(offsetX, 0.0), QPointF(offsetX, rightValue));
        }
    }
}

void QmlWaveformOverview::drawFiltered(QPainter* pPainter,
        Channels channels,
        ConstWaveformPointer pWaveform,
        int completion) const {
    const double offsetX = completion / 2.0;
    const auto drawChannel = [&](int dataIndex, float direction) {
        waveform::WaveformRgbColumn column;
        column.lowAmplitude = pWaveform->getLow(dataIndex);
        column.midAmplitude = pWaveform->getMid(dataIndex);
        column.highAmplitude = pWaveform->getHigh(dataIndex);
        column.unionAmplitude = pWaveform->getAll(dataIndex);
        const auto layers =
                waveform::buildRekordboxOverviewWaveformLayers(column);
        float innerAmplitude = 0.0f;
        for (const waveform::WaveformRgbLayer& layer : layers) {
            const waveform::WaveformRgbColor color =
                    m_threeBandPalette.color(layer.bands);
            pPainter->setPen(QColor(color.red, color.green, color.blue));
            const float outerAmplitude = innerAmplitude + layer.amplitude;
            pPainter->drawLine(QPointF(offsetX, direction * innerAmplitude),
                    QPointF(offsetX, direction * outerAmplitude));
            innerAmplitude = outerAmplitude;
        }
    };

    if (channels.testFlag(ChannelFlag::LeftChannel)) {
        drawChannel(completion, -1.0f);
    }
    if (channels.testFlag(ChannelFlag::RightChannel)) {
        drawChannel(completion + 1, 1.0f);
    }
}

QColor QmlWaveformOverview::getRgbPenColor(ConstWaveformPointer pWaveform, int completion) const {
    // Retrieve "raw" LMH values from waveform
    qreal low = static_cast<qreal>(pWaveform->getLow(completion));
    qreal mid = static_cast<qreal>(pWaveform->getMid(completion));
    qreal high = static_cast<qreal>(pWaveform->getHigh(completion));

    // Do matrix multiplication
    qreal red = low * m_colorLow.redF() + mid * m_colorMid.redF() + high * m_colorHigh.redF();
    qreal green = low * m_colorLow.greenF() + mid * m_colorMid.greenF() +
            high * m_colorHigh.greenF();
    qreal blue = low * m_colorLow.blueF() + mid * m_colorMid.blueF() + high * m_colorHigh.blueF();

    // Normalize and draw
    qreal max = math_max3(red, green, blue);
    if (max > 0.0) {
        QColor color;
        color.setRgbF(
                static_cast<float>(red / max),
                static_cast<float>(green / max),
                static_cast<float>(blue / max));
        return color;
    }
    return QColor();
}

} // namespace qml
} // namespace mixxx
