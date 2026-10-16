#pragma once

#include <QColor>
#include <QVariantMap>
#include <array>
#include <utility>

#include "waveform/renderers/waveformrgbcolumnsampler.h"

namespace mixxx::qml {

// Builds a 3-band layer palette from a QML map such as
// { "low": "#39C5BB", "mid": "#E66A9A", "high": "white", "all": ... }.
// Keys: low, mid, high, lowMid, lowHigh, midHigh, all. Missing or invalid
// entries keep the Rekordbox default color.
inline waveform::WaveformLayerPalette waveformLayerPaletteFromMap(
        const QVariantMap& colors) {
    using namespace waveform;
    static const std::array<std::pair<const char*, std::uint8_t>, 7> kKeys{{
            {"low", kWaveformLowBand},
            {"mid", kWaveformMidBand},
            {"high", kWaveformHighBand},
            {"lowMid", kWaveformLowBand | kWaveformMidBand},
            {"lowHigh", kWaveformLowBand | kWaveformHighBand},
            {"midHigh", kWaveformMidBand | kWaveformHighBand},
            {"all", kWaveformLowBand | kWaveformMidBand | kWaveformHighBand},
    }};
    WaveformLayerPalette palette;
    for (const auto& [key, bands] : kKeys) {
        const QVariant value = colors.value(QString::fromLatin1(key));
        if (!value.isValid()) {
            continue;
        }
        const QColor color = value.metaType().id() == QMetaType::QString
                ? QColor::fromString(value.toString())
                : value.value<QColor>();
        if (!color.isValid()) {
            continue;
        }
        palette.colors[bands] = {static_cast<std::uint8_t>(color.red()),
                static_cast<std::uint8_t>(color.green()),
                static_cast<std::uint8_t>(color.blue())};
    }
    return palette;
}

} // namespace mixxx::qml
