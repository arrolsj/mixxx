#include <gtest/gtest.h>

#include "qml/qmlfxmappinglearning.h"

namespace mixxx {
namespace qml {

TEST(QmlFxMappingLearningTest, ExposesSingleSelectedAndGroup3Actions) {
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
    EXPECT_EQ(QmlFxMappingLearning::supportedActionIds().size(), 18);
    const auto key = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.target.master"));
    EXPECT_EQ(key.group, QStringLiteral("[LastCueFX]"));
    EXPECT_EQ(key.item, QStringLiteral("target_master"));
    EXPECT_FALSE(QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.eq.high")).isValid());
}

TEST(QmlFxMappingLearningTest, Group3ActionsTargetRealEffectSlotControls) {
    const auto toggle = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.group.deck2.slot3.toggle"));
    EXPECT_EQ(toggle.group,
            QStringLiteral("[EffectRack1_EffectUnit2_Effect3]"));
    EXPECT_EQ(toggle.item, QStringLiteral("enabled"));

    const auto adjust = QmlFxMappingLearning::controlKeyForAction(
            QStringLiteral("lastcue.fx.group.deck1.slot2.adjust"));
    EXPECT_EQ(adjust.group,
            QStringLiteral("[EffectRack1_EffectUnit1_Effect2]"));
    EXPECT_EQ(adjust.item, QStringLiteral("meta"));
}

TEST(QmlFxMappingLearningTest, TimeBindingSeparatesRelativeAndAbsoluteControls) {
    MidiOptions selectKnob;
    selectKnob.setFlag(MidiOption::SelectKnob);
    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(selectKnob),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_delta")));

    MidiOptions spread64;
    spread64.setFlag(MidiOption::Spread64);
    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(spread64),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_delta")));

    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(MidiOptions()),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_adjust")));

    MidiOptions fourteenBit;
    fourteenBit.setFlag(MidiOption::FourteenBitMSB);
    EXPECT_EQ(QmlFxMappingLearning::controlKeyForTimeBinding(fourteenBit),
            ConfigKey(QStringLiteral("[LastCueFX]"),
                    QStringLiteral("time_adjust")));
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
}

TEST(QmlFxMappingLearningTest, ReadsOnlyAnExplicitDerivedTopologyMarker) {
    EXPECT_EQ(QmlFxMappingLearning::topologyForMappingDescription(
                      QStringLiteral("Factory SX3 mapping")),
            QString());
    EXPECT_EQ(QmlFxMappingLearning::topologyForMappingDescription(
                      QStringLiteral("LastCUE Box FX-LEARN-01 topology=group3 derived mapping.")),
            QStringLiteral("group3"));
    EXPECT_EQ(QmlFxMappingLearning::topologyForMappingDescription(
                      QStringLiteral("LastCUE Box FX-LEARN-01 topology=single-selected derived mapping.")),
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
    EXPECT_EQ(logical.value(QStringLiteral("logicalLabel")).toString(),
            QStringLiteral("14-BIT CC 16 / 48 CH 3"));
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
    EXPECT_EQ(logical.value(QStringLiteral("logicalLabel")).toString(),
            QStringLiteral("0x90 / 0x41 CH 1"));
}

} // namespace qml
} // namespace mixxx
