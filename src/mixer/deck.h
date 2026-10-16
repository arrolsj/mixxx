#pragma once

#include <QObject>
#include <memory>

#include "mixer/basetrackplayer.h"

class ControlPushButton;

class Deck : public BaseTrackPlayerImpl {
    Q_OBJECT
  public:
    Deck(PlayerManager* pParent,
            UserSettingsPointer pConfig,
            EngineMixer* pMixingEngine,
            EffectsManager* pEffectsManager,
            EngineChannel::ChannelOrientation defaultOrientation,
            const ChannelHandleAndGroup& handleGroup);
    ~Deck() override;

  private:
    // [ChannelN],vinyl_mode: shared jog-wheel vinyl (scratch on touch) switch.
    // Any controller mapping and the UI read and toggle the same control.
    std::unique_ptr<ControlPushButton> m_pVinylMode;
};
