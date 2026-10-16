#include "mixer/deck.h"

#include "control/controlpushbutton.h"
#include "moc_deck.cpp"

Deck::Deck(PlayerManager* pParent,
        UserSettingsPointer pConfig,
        EngineMixer* pMixingEngine,
        EffectsManager* pEffectsManager,
        EngineChannel::ChannelOrientation defaultOrientation,
        const ChannelHandleAndGroup& handleGroup)
        : BaseTrackPlayerImpl(pParent,
                  pConfig,
                  pMixingEngine,
                  pEffectsManager,
                  defaultOrientation,
                  handleGroup,
                  /*defaultMainMix*/ true,
                  /*defaultHeadphones*/ false,
                  /*primaryDeck*/ true) {
    m_pVinylMode = std::make_unique<ControlPushButton>(
            ConfigKey(getGroup(), QStringLiteral("vinyl_mode")));
    m_pVinylMode->setButtonMode(ControlPushButton::TOGGLE);
    // Jog wheels start in vinyl mode, matching the DDJ-SX3 power-on state.
    m_pVinylMode->set(1.0);
}

Deck::~Deck() = default;
