#pragma once

#include <QColor>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTimer>
#include <QVariantMap>

#include "qml/qmlplayerproxy.h"
#include "track/track.h"
#include "waveform/renderers/waveformbeatgridsampler.h"
#include "waveform/renderers/waveformrgbcolumnsampler.h"

class QSGNode;

namespace mixxx::qml {

class QmlViewportWaveform : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(mixxx::qml::QmlPlayerProxy* player READ player WRITE setPlayer
                    NOTIFY playerChanged REQUIRED)
    Q_PROPERTY(double displayPosition READ displayPosition WRITE setDisplayPosition
                    NOTIFY displayPositionChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(double rateRatio READ rateRatio WRITE setRateRatio NOTIFY rateRatioChanged)
    Q_PROPERTY(double playMarkerPosition READ playMarkerPosition WRITE setPlayMarkerPosition
                    NOTIFY playMarkerPositionChanged)
    Q_PROPERTY(QColor axesColor READ axesColor WRITE setAxesColor NOTIFY axesColorChanged)
    Q_PROPERTY(QColor highColor READ highColor WRITE setHighColor NOTIFY highColorChanged)
    Q_PROPERTY(QColor midColor READ midColor WRITE setMidColor NOTIFY midColorChanged)
    Q_PROPERTY(QColor lowColor READ lowColor WRITE setLowColor NOTIFY lowColorChanged)
    Q_PROPERTY(Renderer renderer READ renderer WRITE setRenderer NOTIFY rendererChanged)
    Q_PROPERTY(QVariantMap threeBandColors READ threeBandColors WRITE setThreeBandColors
                    NOTIFY threeBandColorsChanged)
    Q_PROPERTY(bool beatGridVisible READ beatGridVisible WRITE setBeatGridVisible
                    NOTIFY beatGridVisibleChanged)
    Q_PROPERTY(bool beatGridDownbeatsOnly READ beatGridDownbeatsOnly
                    WRITE setBeatGridDownbeatsOnly NOTIFY beatGridDownbeatsOnlyChanged)
    Q_PROPERTY(QColor beatColor READ beatColor WRITE setBeatColor NOTIFY beatColorChanged)
    Q_PROPERTY(QColor downbeatColor READ downbeatColor WRITE setDownbeatColor
                    NOTIFY downbeatColorChanged)
    Q_PROPERTY(double trackWidth READ trackWidth NOTIFY trackWidthChanged)
    Q_PROPERTY(double contentX READ contentX NOTIFY contentXChanged)
    QML_NAMED_ELEMENT(ViewportWaveform)

  public:
    enum class Renderer {
        RGB = 1,
        ThreeBand = 2,
    };
    Q_ENUM(Renderer)

    explicit QmlViewportWaveform(QQuickItem* parent = nullptr);
    ~QmlViewportWaveform() override = default;

    QmlPlayerProxy* player() const;
    void setPlayer(QmlPlayerProxy* pPlayer);

    double displayPosition() const;
    void setDisplayPosition(double position);
    double zoom() const;
    void setZoom(double zoom);
    double rateRatio() const;
    void setRateRatio(double rateRatio);
    double playMarkerPosition() const;
    void setPlayMarkerPosition(double position);

    QColor axesColor() const;
    void setAxesColor(const QColor& color);
    QColor highColor() const;
    void setHighColor(const QColor& color);
    QColor midColor() const;
    void setMidColor(const QColor& color);
    QColor lowColor() const;
    void setLowColor(const QColor& color);
    Renderer renderer() const;
    void setRenderer(Renderer renderer);
    QVariantMap threeBandColors() const;
    void setThreeBandColors(const QVariantMap& colors);
    bool beatGridVisible() const;
    void setBeatGridVisible(bool visible);
    bool beatGridDownbeatsOnly() const;
    void setBeatGridDownbeatsOnly(bool downbeatsOnly);
    QColor beatColor() const;
    void setBeatColor(const QColor& color);
    QColor downbeatColor() const;
    void setDownbeatColor(const QColor& color);

    double trackWidth() const;
    double contentX() const;

  signals:
    void playerChanged();
    void displayPositionChanged();
    void zoomChanged();
    void rateRatioChanged();
    void playMarkerPositionChanged();
    void axesColorChanged();
    void highColorChanged();
    void midColorChanged();
    void lowColorChanged();
    void rendererChanged();
    void threeBandColorsChanged();
    void beatGridVisibleChanged();
    void beatGridDownbeatsOnlyChanged();
    void beatColorChanged();
    void downbeatColorChanged();
    void trackWidthChanged();
    void contentXChanged();

  protected:
    QSGNode* updatePaintNode(
            QSGNode* pOldNode, UpdatePaintNodeData* pUpdatePaintNodeData) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

  private slots:
    void slotTrackLoaded(TrackPointer pTrack);
    void slotTrackLoading(TrackPointer pNewTrack, TrackPointer pOldTrack);
    void slotTrackUnloaded();
    void slotWaveformUpdated();
    void slotWaveformProgressTimer();
    void slotBeatsUpdated();

  private:
    void setCurrentTrack(TrackPointer pTrack);
    void refreshTrackEndPositionFromPlayer();
    void updateProgressTimer();
    bool updateViewportMetrics();

    QPointer<QmlPlayerProxy> m_pPlayer;
    TrackPointer m_pCurrentTrack;
    ConstWaveformPointer m_pWaveform;
    waveform::WaveformBeatGridData m_beatGridData;
    audio::FramePos m_trackEndPosition{audio::kStartFramePos};
    waveform::WaveformRgbViewportMetrics m_viewportMetrics;
    QTimer m_progressTimer;
    int m_lastProgressCompletion{-1};

    double m_devicePixelRatio{1.0};
    double m_displayPosition{0.0};
    double m_zoom{7.0};
    double m_rateRatio{1.0};
    double m_playMarkerPosition{0.5};
    QColor m_axesColor{Qt::white};
    QColor m_highColor{Qt::blue};
    QColor m_midColor{Qt::green};
    QColor m_lowColor{Qt::red};
    Renderer m_renderer{Renderer::RGB};
    QVariantMap m_threeBandColors;
    waveform::WaveformLayerPalette m_threeBandPalette;
    bool m_beatGridVisible{false};
    bool m_beatGridDownbeatsOnly{false};
    QColor m_beatColor{Qt::white};
    QColor m_downbeatColor{Qt::white};
};

} // namespace mixxx::qml
