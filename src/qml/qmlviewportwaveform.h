#pragma once

#include <QColor>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>

#include "qml/qmlplayerproxy.h"
#include "track/track.h"
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
    Q_PROPERTY(double highGain READ highGain WRITE setHighGain NOTIFY highGainChanged)
    Q_PROPERTY(double midGain READ midGain WRITE setMidGain NOTIFY midGainChanged)
    Q_PROPERTY(double lowGain READ lowGain WRITE setLowGain NOTIFY lowGainChanged)
    Q_PROPERTY(double allGain READ allGain WRITE setAllGain NOTIFY allGainChanged)
    Q_PROPERTY(double trackWidth READ trackWidth NOTIFY trackWidthChanged)
    Q_PROPERTY(double contentX READ contentX NOTIFY contentXChanged)
    QML_NAMED_ELEMENT(ViewportWaveform)

  public:
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

    double highGain() const;
    void setHighGain(double gain);
    double midGain() const;
    void setMidGain(double gain);
    double lowGain() const;
    void setLowGain(double gain);
    double allGain() const;
    void setAllGain(double gain);

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
    void highGainChanged();
    void midGainChanged();
    void lowGainChanged();
    void allGainChanged();
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

  private:
    void setCurrentTrack(TrackPointer pTrack);
    bool updateViewportMetrics();

    QPointer<QmlPlayerProxy> m_pPlayer;
    TrackPointer m_pCurrentTrack;
    ConstWaveformPointer m_pWaveform;
    waveform::WaveformRgbViewportMetrics m_viewportMetrics;

    double m_devicePixelRatio{1.0};
    double m_displayPosition{0.0};
    double m_zoom{7.0};
    double m_rateRatio{1.0};
    double m_playMarkerPosition{0.5};
    QColor m_axesColor{Qt::white};
    QColor m_highColor{Qt::blue};
    QColor m_midColor{Qt::green};
    QColor m_lowColor{Qt::red};
    double m_highGain{1.0};
    double m_midGain{1.0};
    double m_lowGain{1.0};
    double m_allGain{1.0};
};

} // namespace mixxx::qml
