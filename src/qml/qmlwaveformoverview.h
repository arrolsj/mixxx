#pragma once

#include <QPainter>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>

#include "qml/qmlplayerproxy.h"
#include "track/track.h"
#include "waveform/renderers/waveformrgbcolumnsampler.h"

namespace mixxx {
namespace qml {

class QmlWaveformOverview : public QQuickPaintedItem {
    Q_OBJECT
    Q_FLAGS(Channels)
    Q_PROPERTY(mixxx::qml::QmlPlayerProxy* player READ getPlayer WRITE setPlayer
                    NOTIFY playerChanged REQUIRED)
    Q_PROPERTY(Channels channels READ getChannels WRITE setChannels NOTIFY channelsChanged)
    Q_PROPERTY(Renderer renderer READ getRenderer WRITE setRenderer NOTIFY rendererChanged)
    Q_PROPERTY(QColor colorHigh READ getColorHigh WRITE setColorHigh NOTIFY colorHighChanged)
    Q_PROPERTY(QColor colorMid READ getColorMid WRITE setColorMid NOTIFY colorMidChanged)
    Q_PROPERTY(QColor colorLow READ getColorLow WRITE setColorLow NOTIFY colorLowChanged)
    Q_PROPERTY(double highGain READ getHighGain WRITE setHighGain NOTIFY highGainChanged)
    Q_PROPERTY(double midGain READ getMidGain WRITE setMidGain NOTIFY midGainChanged)
    Q_PROPERTY(double lowGain READ getLowGain WRITE setLowGain NOTIFY lowGainChanged)
    QML_NAMED_ELEMENT(WaveformOverview)

  public:
    enum class ChannelFlag : int {
        LeftChannel = 1,
        RightChannel = 2,
        BothChannels = LeftChannel | RightChannel,
    };
    Q_DECLARE_FLAGS(Channels, ChannelFlag)

    enum class Renderer {
        RGB = 1,
        ThreeBand = 2,
        Filtered = ThreeBand,
    };
    Q_ENUM(Renderer)

    QmlWaveformOverview(QQuickItem* parent = nullptr);
    ~QmlWaveformOverview() override = default;

    void paint(QPainter* painter) override;

    void setPlayer(QmlPlayerProxy* player);
    QmlPlayerProxy* getPlayer() const;

    void setChannels(Channels channels);
    Channels getChannels() const;
    Renderer getRenderer() const;
    void setRenderer(Renderer renderer);
    QColor getColorHigh() const;
    void setColorHigh(const QColor& color);
    QColor getColorMid() const;
    void setColorMid(const QColor& color);
    QColor getColorLow() const;
    void setColorLow(const QColor& color);
    double getHighGain() const;
    void setHighGain(double gain);
    double getMidGain() const;
    void setMidGain(double gain);
    double getLowGain() const;
    void setLowGain(double gain);

  private slots:
    void slotTrackLoaded(TrackPointer pLoadedTrack);
    void slotTrackLoading(TrackPointer pNewTrack, TrackPointer pOldTrack);
    void slotTrackUnloaded();
    void slotWaveformUpdated();

  signals:
    void playerChanged();
    void channelsChanged(mixxx::qml::QmlWaveformOverview::Channels channels);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    void rendererChanged(Renderer renderer);
#else
    void rendererChanged(mixxx::qml::QmlWaveformOverview::Renderer renderer);
#endif
    void colorHighChanged(const QColor& color);
    void colorMidChanged(const QColor& color);
    void colorLowChanged(const QColor& color);
    void highGainChanged();
    void midGainChanged();
    void lowGainChanged();

  private:
    void setCurrentTrack(TrackPointer pTrack);
    void drawFiltered(QPainter* pPainter,
            Channels channels,
            ConstWaveformPointer pWaveform,
            int completion) const;
    void drawRgb(QPainter* pPainter,
            Channels channels,
            ConstWaveformPointer pWaveform,
            int completion) const;
    QColor getRgbPenColor(ConstWaveformPointer pWaveform, int completion) const;
    void drawRekordboxThreeBand(QPainter* pPainter,
            Channels channels,
            const mixxx::waveform::RekordboxThreeBandColumn& column,
            double offsetX,
            const mixxx::waveform::WaveformThreeBandReference& reference,
            bool previewProfile) const;

    QPointer<QmlPlayerProxy> m_pPlayer;
    TrackPointer m_pCurrentTrack;
    Channels m_channels;
    Renderer m_renderer;
    QColor m_colorHigh;
    QColor m_colorMid;
    QColor m_colorLow;
    mixxx::waveform::RekordboxThreeBandWaveformsPointer
            m_pRekordboxThreeBandWaveforms;
    mixxx::waveform::WaveformThreeBandReference m_rekordboxDetailReference{
            mixxx::waveform::kMixxxThreeBandReference};
    double m_highGain{1.0};
    double m_midGain{1.0};
    double m_lowGain{1.0};
};

} // namespace qml
} // namespace mixxx
