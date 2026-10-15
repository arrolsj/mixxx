#include <gtest/gtest.h>

#include "qml/qmlfxmappinglearning.h"

namespace mixxx {
namespace qml {

TEST(QmlFxMappingLearningTest, ExposesCompactGeneralAndFxActionSets) {
    EXPECT_EQ(QmlFxMappingLearning::generalActionIds().size(), 17);
    EXPECT_EQ(QmlFxMappingLearning::generalActionIds().first(),
            QStringLiteral("lastcue.general.back"));
    EXPECT_EQ(QmlFxMappingLearning::generalActionIds().last(),
            QStringLiteral("lastcue.general.deck2.beatjump.forward"));

    EXPECT_EQ(QmlFxMappingLearning::singleSelectedActionIds(),
            QStringList({
                    QStringLiteral("lastcue.fx.select_delta"),
                    QStringLiteral("lastcue.fx.toggle_selected"),
                    QStringLiteral("lastcue.fx.target.deck1"),
                    QStringLiteral("lastcue.fx.target.deck2"),
                    QStringLiteral("lastcue.fx.target.master"),
                    QStringLiteral("lastcue.fx.time_adjust"),
            }));
    EXPECT_EQ(QmlFxMappingLearning::group3ActionIds().size(), 13);
    EXPECT_EQ(QmlFxMappingLearning::supportedActionIds().size(), 35);
}

TEST(QmlFxMappingLearningTest, GeneralActionsRoundTripThroughRealAndBoxControls) {
    const auto sync = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.general.deck1.sync"));
    EXPECT_EQ(sync, ConfigKey(QStringLiteral("[Channel1]"),
                            QStringLiteral("sync_enabled")));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForControlKey(sync),
            QStringLiteral("lastcue.general.deck1.sync"));

    const auto tempoRange = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.general.deck2.tempo_range"));
    EXPECT_EQ(tempoRange, ConfigKey(QStringLiteral("[LastCueGeneral]"),
                                  QStringLiteral("tempo_range_deck2")));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForControlKey(tempoRange),
            QStringLiteral("lastcue.general.deck2.tempo_range"));

    EXPECT_FALSE(QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.eq.high")).isValid());
}

TEST(QmlFxMappingLearningTest,
        RecognizesHighConfidenceNativeScriptBehaviorFingerprints) {
    const auto scriptMapping = [](unsigned char status,
                                       unsigned char control,
                                       const QString& group,
                                       const QString& item) {
        return MidiInputMapping(MidiKey(status, control),
                MidiOptions(MidiOption::Script),
                ConfigKey(group, item));
    };

    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0xB6,
                      0x40,
                      QStringLiteral("[Library]"),
                      QStringLiteral("PioneerDDJSX.rotarySelector"))),
            QStringLiteral("lastcue.general.browse.turn"));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0x90,
                      0x24,
                      QStringLiteral("[Channel1]"),
                      QStringLiteral("PioneerDDJSX.parameterLeft"))),
            QStringLiteral("lastcue.general.deck1.beatjump.backward"));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0x91,
                      0x60,
                      QStringLiteral("[Channel2]"),
                      QStringLiteral("PioneerDDJSX.shiftKeyLockButton"))),
            QStringLiteral("lastcue.general.deck2.tempo_range"));

    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0x94,
                      0x47,
                      QStringLiteral("[EffectRack1_EffectUnit1]"),
                      QStringLiteral("PioneerDDJSX.effectUnit[1].enableButtons[1].input"))),
            QStringLiteral("lastcue.fx.group.deck1.slot1.toggle"));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0xB5,
                      0x06,
                      QStringLiteral("[EffectRack1_EffectUnit2]"),
                      QStringLiteral("PioneerDDJSX.effectUnit[2].knobs[3].inputMSB"))),
            QStringLiteral("lastcue.fx.group.deck2.slot3.adjust"));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0xB5,
                      0x26,
                      QStringLiteral("[EffectRack1_EffectUnit2]"),
                      QStringLiteral("PioneerDDJSX.effectUnit[2].knobs[3].inputLSB"))),
            QStringLiteral("lastcue.fx.group.deck2.slot3.adjust"));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0xB4,
                      0x00,
                      QStringLiteral("[EffectRack1_EffectUnit1]"),
                      QStringLiteral("PioneerDDJSX.effectUnit[1].dryWetKnob.input"))),
            QStringLiteral("lastcue.fx.time_adjust"));

    EXPECT_TRUE(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0xB5,
                                                                 0x16,
                                                                 QStringLiteral("[EffectRack1_EffectUnit2]"),
                                                                 QStringLiteral("PioneerDDJSX.effectUnit[2].knobs[3].inputMSB")))
                    .isEmpty());
    EXPECT_TRUE(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0xB4,
                                                                 0x10,
                                                                 QStringLiteral("[EffectRack1_EffectUnit1]"),
                                                                 QStringLiteral("PioneerDDJSX.effectUnit[1].dryWetKnob.input")))
                    .isEmpty());
    EXPECT_TRUE(QmlFxMappingLearning::actionIdForMapping(scriptMapping(0x90,
                                                                 0x26,
                                                                 QStringLiteral("[Channel1]"),
                                                                 QStringLiteral("PioneerDDJSX.parameterLeft")))
                    .isEmpty());
}

TEST(QmlFxMappingLearningTest, Group3ActionsTargetRealEffectSlotControls) {
    const auto toggle = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.group.deck2.slot3.toggle"));
    EXPECT_EQ(toggle.group,
            QStringLiteral("[EffectRack1_EffectUnit2_Effect3]"));
    EXPECT_EQ(toggle.item, QStringLiteral("enabled"));
    EXPECT_EQ(QmlFxMappingLearning::actionIdForControlKey(toggle),
            QStringLiteral("lastcue.fx.group.deck2.slot3.toggle"));

    const auto adjust = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.group.deck1.slot2.adjust"));
    EXPECT_EQ(adjust.group,
            QStringLiteral("[EffectRack1_EffectUnit1_Effect2]"));
    EXPECT_EQ(adjust.item, QStringLiteral("meta"));
}

TEST(QmlFxMappingLearningTest, TimeBindingPreservesRelativeAndAbsoluteBehavior) {
    MidiOptions selectKnob;
    selectKnob.setFlag(MidiOption::SelectKnob);
    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(selectKnob),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_delta")));
    EXPECT_EQ(QmlFxMappingLearning::behaviorKindForOptions(selectKnob),
            QStringLiteral("relative"));

    MidiOptions spread64;
    spread64.setFlag(MidiOption::Spread64);
    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(spread64),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_delta")));
    EXPECT_EQ(QmlFxMappingLearning::behaviorKindForOptions(spread64),
            QStringLiteral("relative"));

    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(MidiOptions()),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_adjust")));
    EXPECT_EQ(QmlFxMappingLearning::behaviorKindForOptions(MidiOptions()),
            QStringLiteral("absolute"));

    MidiOptions fourteenBit;
    fourteenBit.setFlag(MidiOption::FourteenBitMSB);
    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(fourteenBit),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_adjust")));
    EXPECT_EQ(QmlFxMappingLearning::behaviorKindForOptions(fourteenBit),
            QStringLiteral("high-resolution"));
}

TEST(QmlFxMappingLearningTest, RecommendsNativeGroup3ForSx3Identity) {
    EXPECT_TRUE(QmlFxMappingLearning::isKnownNativeGroup3Controller(
            QStringLiteral("PIONEER DJ DDJ-SX3"), QString()));
    EXPECT_TRUE(QmlFxMappingLearning::isKnownNativeGroup3Controller(
            QString(), QStringLiteral("Pioneer DDJ-SX3 Experimental")));
    EXPECT_TRUE(QmlFxMappingLearning::isKnownNativeGroup3Controller(
            QStringLiteral("Pioneer DDJ-SX"),
            QStringLiteral("Pioneer DDJ-SX")));
    EXPECT_FALSE(QmlFxMappingLearning::isKnownNativeGroup3Controller(
            QStringLiteral("Pioneer DDJ-400"),
            QStringLiteral("Pioneer DDJ-400")));
    EXPECT_EQ(QmlFxMappingLearning::factoryPresetDisplayName(
                      QStringLiteral("Pioneer DDJ-SX3 Experimental")),
            QStringLiteral("Pioneer DDJ-SX3"));
}

TEST(QmlFxMappingLearningTest, ReadsOnlyAnExplicitDerivedTopologyMarker) {
    EXPECT_EQ(QmlFxMappingLearning::topologyForMappingDescription(
                      QStringLiteral("Factory SX3 mapping")),
            QString());
    EXPECT_EQ(QmlFxMappingLearning::topologyForMappingDescription(
                      QStringLiteral("LastCUE Box FX-LEARN-01 topology=group3 derived mapping.")),
            QStringLiteral("group3"));
    EXPECT_EQ(QmlFxMappingLearning::topologyForMappingDescription(
                      QStringLiteral("LastCUE Box MAP-01 kind=fx topology=single-selected derived mapping.")),
            QStringLiteral("single-selected"));
}

TEST(QmlFxMappingLearningTest, CollapsesFourteenBitPairIntoOneLogicalBinding) {
    MidiOptions msbOptions;
    msbOptions.setFlag(MidiOption::FourteenBitMSB);
    MidiOptions lsbOptions;
    lsbOptions.setFlag(MidiOption::FourteenBitLSB);
    const auto target = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.time_adjust"));
    const MidiInputMappings mappings = {
            MidiInputMapping(MidiKey(0xB2, 0x10), msbOptions, target),
            MidiInputMapping(MidiKey(0xB2, 0x30), lsbOptions, target),
    };

    const auto logical =
            QmlFxMappingLearning::describeLogicalBinding(mappings);
    EXPECT_TRUE(logical.value(QStringLiteral("is14Bit")).toBool());
    EXPECT_EQ(logical.value(QStringLiteral("msbControl")).toInt(), 0x10);
    EXPECT_EQ(logical.value(QStringLiteral("lsbControl")).toInt(), 0x30);
    EXPECT_EQ(logical.value(QStringLiteral("behaviorKind")).toString(),
            QStringLiteral("high-resolution"));
    EXPECT_EQ(logical.value(QStringLiteral("logicalLabel")).toString(),
            QStringLiteral("HIGH RES KNOB · CH 3 · CC 16+48"));
}

TEST(QmlFxMappingLearningTest, DescribesButtonPairAsOneLogicalBinding) {
    const auto target = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.toggle_selected"));
    const MidiInputMappings mappings = {
            MidiInputMapping(MidiKey(0x90, 0x41), MidiOptions(), target),
            MidiInputMapping(MidiKey(0x80, 0x41), MidiOptions(), target),
    };

    const auto logical =
            QmlFxMappingLearning::describeLogicalBinding(mappings);
    EXPECT_FALSE(logical.value(QStringLiteral("is14Bit")).toBool());
    EXPECT_EQ(logical.value(QStringLiteral("behaviorKind")).toString(),
            QStringLiteral("button"));
    EXPECT_EQ(logical.value(QStringLiteral("logicalLabel")).toString(),
            QStringLiteral("CH 1 · 0x90/0x41"));
}

TEST(QmlFxMappingLearningTest, SuppressedScriptBindingNeverCallsAScript) {
    // A learned SX3 FX time knob replaces a script binding. The no-op that
    // hides it must not keep the Script option, or every message calls an
    // undefined "suppressed" function and Mixxx opens its error dialog.
    MidiOptions scriptOptions(MidiOption::Script);
    scriptOptions.setFlag(MidiOption::SelectKnob);
    const MidiInputMapping source(MidiKey(0xB4, 0x00),
            scriptOptions,
            ConfigKey(QStringLiteral("[EffectRack1_EffectUnit1]"),
                    QStringLiteral("PioneerDDJSX.effectUnit[1].dryWetKnob.input")));

    const auto suppressed = QmlFxMappingLearning::suppressedMappingFor(source);
    EXPECT_EQ(suppressed.key.key, source.key.key);
    EXPECT_EQ(suppressed.options, MidiOptions());
    EXPECT_FALSE(suppressed.options.testFlag(MidiOption::Script));
    ASSERT_TRUE(std::holds_alternative<ConfigKey>(suppressed.control));
    EXPECT_EQ(std::get<ConfigKey>(suppressed.control),
            ConfigKey(QStringLiteral("[LastCueMapping]"),
                    QStringLiteral("suppressed")));
}

TEST(QmlFxMappingLearningTest, CachedCatalogMarksOnlyTheActiveMappingCurrent) {
    const QList<QVariantMap> catalog = {
            {{QStringLiteral("path"), QStringLiteral("/opt/mappings/SX3.midi.xml")}},
            {{QStringLiteral("path"), QStringLiteral("/profile/controllers/My SX3.midi.xml")}},
    };

    const auto marked = QmlFxMappingLearning::catalogWithCurrent(
            catalog, QStringLiteral("/profile/controllers/./My SX3.midi.xml"));
    ASSERT_EQ(marked.size(), 2);
    EXPECT_FALSE(marked[0].toMap().value(QStringLiteral("current")).toBool());
    EXPECT_TRUE(marked[1].toMap().value(QStringLiteral("current")).toBool());

    const auto none = QmlFxMappingLearning::catalogWithCurrent(catalog, QString());
    for (const auto& item : none) {
        EXPECT_FALSE(item.toMap().value(QStringLiteral("current")).toBool());
    }
}

} // namespace qml
} // namespace mixxx
