#include "qml/qmlwaveformoverview.h"

#include <algorithm>

#include "mixer/basetrackplayer.h"
#include "waveform/renderers/waveformrgbcolumnsampler.h"
#include "moc_qmlwaveformoverview.cpp"

namespace {
constexpr double kDesiredChannelHeight = 255;
constexpr int kLowBandAlpha = 230;
constexpr int kMidBandAlpha = 218;
constexpr int kHighBandAlpha = 242;

QColor withAlpha(const QColor& color, int alpha) {
    QColor result = color;
    result.setAlpha(alpha);
    return result;
}
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
          m_colorLow(0x0000FF) {
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
            update();
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
    update();
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
    update();
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
    update();
}

QColor QmlWaveformOverview::getColorHigh() const {
    return m_colorHigh;
}

void QmlWaveformOverview::setColorHigh(const QColor& color) {
    if (m_colorHigh == color) {
        return;
    }
    m_colorHigh = color;
    emit colorHighChanged(color);
    update();
}

QColor QmlWaveformOverview::getColorMid() const {
    return m_colorMid;
}

void QmlWaveformOverview::setColorMid(const QColor& color) {
    if (m_colorMid == color) {
        return;
    }
    m_colorMid = color;
    emit colorMidChanged(color);
    update();
}

QColor QmlWaveformOverview::getColorLow() const {
    return m_colorLow;
}

void QmlWaveformOverview::setColorLow(const QColor& color) {
    if (m_colorLow == color) {
        return;
    }
    m_colorLow = color;
    emit colorLowChanged(color);
    update();
}

double QmlWaveformOverview::getHighGain() const {
    return m_highGain;
}

void QmlWaveformOverview::setHighGain(double gain) {
    const double normalized = std::max(gain, 0.0);
    if (qFuzzyCompare(m_highGain + 1.0, normalized + 1.0)) {
        return;
    }
    m_highGain = normalized;
    emit highGainChanged();
    update();
}

double QmlWaveformOverview::getMidGain() const {
    return m_midGain;
}

void QmlWaveformOverview::setMidGain(double gain) {
    const double normalized = std::max(gain, 0.0);
    if (qFuzzyCompare(m_midGain + 1.0, normalized + 1.0)) {
        return;
    }
    m_midGain = normalized;
    emit midGainChanged();
    update();
}

double QmlWaveformOverview::getLowGain() const {
    return m_lowGain;
}

void QmlWaveformOverview::setLowGain(double gain) {
    const double normalized = std::max(gain, 0.0);
    if (qFuzzyCompare(m_lowGain + 1.0, normalized + 1.0)) {
        return;
    }
    m_lowGain = normalized;
    emit lowGainChanged();
    update();
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
    if (pTrack != nullptr) {
        connect(pTrack.get(),
                &Track::waveformSummaryUpdated,
                this,
                &QmlWaveformOverview::slotWaveformUpdated);
        connect(pTrack.get(),
                &Track::rekordboxThreeBandWaveformsUpdated,
                this,
                &QmlWaveformOverview::slotWaveformUpdated);
    }
    slotWaveformUpdated();
}

void QmlWaveformOverview::slotWaveformUpdated() {
    m_pRekordboxThreeBandWaveforms = m_pCurrentTrack != nullptr
            ? m_pCurrentTrack->getRekordboxThreeBandWaveforms()
            : mixxx::waveform::RekordboxThreeBandWaveformsPointer{};
    m_rekordboxPreviewReference = m_pRekordboxThreeBandWaveforms
            ? mixxx::waveform::calculateRekordboxThreeBandReference(
                      m_pRekordboxThreeBandWaveforms->preview)
            : mixxx::waveform::kMixxxThreeBandReference;
    m_rekordboxDetailReference = m_pRekordboxThreeBandWaveforms
            ? mixxx::waveform::calculateRekordboxThreeBandReference(
                      m_pRekordboxThreeBandWaveforms->detail)
            : mixxx::waveform::kMixxxThreeBandReference;
    update();
}

void QmlWaveformOverview::paint(QPainter* pPainter) {
    TrackPointer pTrack = m_pCurrentTrack;
    if (!pTrack) {
        return;
    }

    const mixxx::waveform::RekordboxThreeBandWaveformsPointer pRekordbox =
            m_pRekordboxThreeBandWaveforms;
    const std::vector<mixxx::waveform::RekordboxThreeBandColumn>*
            pRekordboxColumns = nullptr;
    float rekordboxReference = mixxx::waveform::kMixxxThreeBandReference;
    if (m_renderer == Renderer::ThreeBand && pRekordbox &&
            !pRekordbox->empty()) {
        const bool usePreview = !pRekordbox->preview.empty();
        pRekordboxColumns = usePreview
                ? &pRekordbox->preview
                : &pRekordbox->detail;
        rekordboxReference = usePreview
                ? m_rekordboxPreviewReference
                : m_rekordboxDetailReference;
    }
    ConstWaveformPointer pWaveform = pTrack->getWaveformSummary();
    if (!pWaveform && pRekordboxColumns == nullptr) {
        return;
    }

    const int dataSize = pRekordboxColumns != nullptr
            ? static_cast<int>(pRekordboxColumns->size()) * 2
            : pWaveform->getDataSize();
    if (dataSize == 0) {
        return;
    }

    constexpr int actualCompletion = 0;
    // Always multiple of 2
    const int waveformCompletion = pRekordboxColumns != nullptr
            ? dataSize
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

    const double desiredChannelHeight = m_renderer == Renderer::ThreeBand
            ? 1.0
            : kDesiredChannelHeight;

    const Channels channels = m_channels;
    pPainter->save();

    switch (channels) {
    case static_cast<int>(ChannelFlag::LeftChannel):
        // Draw both channels.
        // Set the y axis to half the height of the item
        pPainter->translate(0.0, height());
        // Set the x axis to half the height of the item
        pPainter->scale(width() / desiredWidth, height() / desiredChannelHeight);
        break;
    case static_cast<int>(ChannelFlag::RightChannel):
        // Set the x axis to half the height of the item
        pPainter->scale(width() / desiredWidth, height() / desiredChannelHeight);
        break;
    default:
        // Draw both channels.
        // Set the y axis to half the height of the item
        pPainter->translate(0.0, height() / 2);
        // Set the x axis to half the height of the item
        pPainter->scale(width() / desiredWidth, height() / (2 * desiredChannelHeight));
    }

    if (pRekordboxColumns != nullptr) {
        for (std::size_t index = 0; index < pRekordboxColumns->size(); ++index) {
            drawRekordboxThreeBand(pPainter,
                    channels,
                    (*pRekordboxColumns)[index],
                    static_cast<double>(index),
                    rekordboxReference);
        }
        pPainter->restore();
        return;
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
    const auto displayAmplitude = [](std::uint8_t value) {
        return mixxx::waveform::threeBandDisplayAmplitude(
                value, mixxx::waveform::kMixxxThreeBandReference);
    };

    if (channels.testFlag(ChannelFlag::LeftChannel)) {
        pPainter->setPen(withAlpha(m_colorLow, kLowBandAlpha));
        pPainter->drawLine(QPointF(offsetX,
                                   -displayAmplitude(
                                           pWaveform->getLow(completion)) *
                                           m_lowGain),
                QPointF(offsetX, 0.0));
        pPainter->setPen(withAlpha(m_colorMid, kMidBandAlpha));
        pPainter->drawLine(QPointF(offsetX,
                                   -displayAmplitude(
                                           pWaveform->getMid(completion)) *
                                           m_midGain),
                QPointF(offsetX, 0.0));
        pPainter->setPen(withAlpha(m_colorHigh, kHighBandAlpha));
        pPainter->drawLine(QPointF(offsetX,
                                   -displayAmplitude(
                                           pWaveform->getHigh(completion)) *
                                           m_highGain),
                QPointF(offsetX, 0.0));
    }

    if (channels.testFlag(ChannelFlag::RightChannel)) {
        pPainter->setPen(withAlpha(m_colorLow, kLowBandAlpha));
        pPainter->drawLine(QPointF(offsetX, 0.0),
                QPointF(offsetX,
                        displayAmplitude(pWaveform->getLow(completion + 1)) *
                                m_lowGain));
        pPainter->setPen(withAlpha(m_colorMid, kMidBandAlpha));
        pPainter->drawLine(QPointF(offsetX, 0.0),
                QPointF(offsetX,
                        displayAmplitude(pWaveform->getMid(completion + 1)) *
                                m_midGain));
        pPainter->setPen(withAlpha(m_colorHigh, kHighBandAlpha));
        pPainter->drawLine(QPointF(offsetX, 0.0),
                QPointF(offsetX,
                        displayAmplitude(pWaveform->getHigh(completion + 1)) *
                                m_highGain));
    }
}

void QmlWaveformOverview::drawRekordboxThreeBand(
        QPainter* pPainter,
        Channels channels,
        const mixxx::waveform::RekordboxThreeBandColumn& column,
        double offsetX,
        float reference) const {
    const double lowHeight =
            mixxx::waveform::threeBandDisplayAmplitude(column.low, reference) *
            m_lowGain;
    const double midHeight =
            mixxx::waveform::threeBandDisplayAmplitude(column.mid, reference) *
            m_midGain;
    const double highHeight =
            mixxx::waveform::threeBandDisplayAmplitude(column.high, reference) *
            m_highGain;

    if (channels.testFlag(ChannelFlag::LeftChannel)) {
        pPainter->setPen(withAlpha(m_colorLow, kLowBandAlpha));
        pPainter->drawLine(QPointF(offsetX, -lowHeight),
                QPointF(offsetX, 0.0));
        pPainter->setPen(withAlpha(m_colorMid, kMidBandAlpha));
        pPainter->drawLine(QPointF(offsetX, -midHeight),
                QPointF(offsetX, 0.0));
        pPainter->setPen(withAlpha(m_colorHigh, kHighBandAlpha));
        pPainter->drawLine(QPointF(offsetX, -highHeight),
                QPointF(offsetX, 0.0));
    }

    if (channels.testFlag(ChannelFlag::RightChannel)) {
        pPainter->setPen(withAlpha(m_colorLow, kLowBandAlpha));
        pPainter->drawLine(QPointF(offsetX, 0.0),
                QPointF(offsetX, lowHeight));
        pPainter->setPen(withAlpha(m_colorMid, kMidBandAlpha));
        pPainter->drawLine(QPointF(offsetX, 0.0),
                QPointF(offsetX, midHeight));
        pPainter->setPen(withAlpha(m_colorHigh, kHighBandAlpha));
        pPainter->drawLine(QPointF(offsetX, 0.0),
                QPointF(offsetX, highHeight));
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
