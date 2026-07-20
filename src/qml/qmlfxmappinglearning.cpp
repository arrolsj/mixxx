#include "qml/qmlfxmappinglearning.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QThread>
#include <algorithm>
#include <utility>
#include <variant>

#include "control/controlencoder.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "controllers/controllermanager.h"
#include "controllers/controllermappinginfoenumerator.h"
#include "controllers/defs_controllers.h"
#include "controllers/learningutils.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#include "controllers/midi/legacymidicontrollermapping.h"
#include "controllers/midi/midicontroller.h"
#include "controllers/midi/midiutils.h"
#include "moc_qmlfxmappinglearning.cpp"
#include "util/assert.h"

namespace mixxx {
namespace qml {
namespace {
const QString kFxControlGroup = QStringLiteral("[LastCueFX]");
const QString kGeneralControlGroup = QStringLiteral("[LastCueGeneral]");
const QString kMappingControlGroup = QStringLiteral("[LastCueMapping]");
const QString kStateIdle = QStringLiteral("idle");
const QString kStateListening = QStringLiteral("listening");
const QString kStateSuggested = QStringLiteral("suggested");
const QString kStateCandidate = QStringLiteral("candidate");
const QString kStateReady = QStringLiteral("ready");
const QString kStateReview = QStringLiteral("review");
const QString kStateSaved = QStringLiteral("saved");
const QString kConflictNone = QStringLiteral("none");
const QString kConflictExistingMapping = QStringLiteral("existing-mapping");
const QString kConflictBoxAction = QStringLiteral("box-action");
const QString kSessionGeneral = QStringLiteral("general");
const QString kSessionFx = QStringLiteral("fx");

const QList<QPair<QString, ConfigKey>> kGeneralActionControls = {
        {QStringLiteral("lastcue.general.back"),
                ConfigKey(QStringLiteral("[Library]"), QStringLiteral("MoveFocusBackward"))},
        {QStringLiteral("lastcue.general.browse.turn"),
                ConfigKey(QStringLiteral("[Library]"), QStringLiteral("MoveVertical"))},
        {QStringLiteral("lastcue.general.browse.press"),
                ConfigKey(QStringLiteral("[Library]"), QStringLiteral("GoToItem"))},
        {QStringLiteral("lastcue.general.load.deck1"),
                ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("LoadSelectedTrack"))},
        {QStringLiteral("lastcue.general.load.deck2"),
                ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("LoadSelectedTrack"))},
        {QStringLiteral("lastcue.general.deck1.sync"),
                ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("sync_enabled"))},
        {QStringLiteral("lastcue.general.deck2.sync"),
                ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("sync_enabled"))},
        {QStringLiteral("lastcue.general.deck1.quantize"),
                ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("quantize"))},
        {QStringLiteral("lastcue.general.deck2.quantize"),
                ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("quantize"))},
        {QStringLiteral("lastcue.general.deck1.tempo_range"),
                ConfigKey(kGeneralControlGroup, QStringLiteral("tempo_range_deck1"))},
        {QStringLiteral("lastcue.general.deck2.tempo_range"),
                ConfigKey(kGeneralControlGroup, QStringLiteral("tempo_range_deck2"))},
        {QStringLiteral("lastcue.general.deck1.keylock"),
                ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("keylock"))},
        {QStringLiteral("lastcue.general.deck2.keylock"),
                ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("keylock"))},
        {QStringLiteral("lastcue.general.deck1.beatjump.backward"),
                ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("beatjump_backward"))},
        {QStringLiteral("lastcue.general.deck1.beatjump.forward"),
                ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("beatjump_forward"))},
        {QStringLiteral("lastcue.general.deck2.beatjump.backward"),
                ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("beatjump_backward"))},
        {QStringLiteral("lastcue.general.deck2.beatjump.forward"),
                ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("beatjump_forward"))},
};

const QList<QPair<QString, QString>> kSingleSelectedActionControls = {
        {QStringLiteral("lastcue.fx.select_delta"), QStringLiteral("select_delta")},
        {QStringLiteral("lastcue.fx.toggle_selected"), QStringLiteral("toggle_selected")},
        {QStringLiteral("lastcue.fx.target.deck1"), QStringLiteral("target_deck1")},
        {QStringLiteral("lastcue.fx.target.deck2"), QStringLiteral("target_deck2")},
        {QStringLiteral("lastcue.fx.target.master"), QStringLiteral("target_master")},
        {QStringLiteral("lastcue.fx.time_adjust"), QStringLiteral("time_adjust")},
};

const QStringList kFactoryPresetNames = {
        QStringLiteral("Pioneer DDJ-FLX4"),
        QStringLiteral("Pioneer DDJ-400"),
        QStringLiteral("Pioneer DDJ-SX3 Experimental"),
};

const QString kTopologySingleSelected = QStringLiteral("single-selected");
const QString kTopologyGroup3 = QStringLiteral("group3");
const QString kTimeAdjustControl = QStringLiteral("time_adjust");
const QString kTimeDeltaControl = QStringLiteral("time_delta");
const QString kSuppressedControl = QStringLiteral("suppressed");

QString hexByte(unsigned char value) {
    return QStringLiteral("0x%1")
            .arg(QString::number(value, 16).toUpper().rightJustified(2, QChar('0')));
}

bool isRelativeOptions(MidiOptions options) {
    return options.testFlag(MidiOption::SelectKnob) ||
            options.testFlag(MidiOption::Spread64) ||
            options.testFlag(MidiOption::Rot64) ||
            options.testFlag(MidiOption::Rot64Invert) ||
            options.testFlag(MidiOption::Rot64Fast) ||
            options.testFlag(MidiOption::Diff) ||
            options.testFlag(MidiOption::HercJog) ||
            options.testFlag(MidiOption::HercJogFast);
}

QString normalizedPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}
} // namespace

QmlFxMappingLearning::QmlFxMappingLearning(
        ControllerManager* pManager,
        UserSettingsPointer pConfig,
        QObject* pParent)
        : QObject(pParent),
          m_pManager(pManager),
          m_pConfig(std::move(pConfig)),
          m_currentActionIndex(-1),
          m_available(false),
          m_sessionActive(false),
          m_candidateInherited(false),
          m_replaceCurrentActionOnAccept(false),
          m_state(kStateIdle),
          m_conflictKind(kConflictNone),
          m_recommendedTopology(kTopologySingleSelected),
          m_nativeGroup3Available(false),
          m_pSelectDeltaControl(std::make_unique<ControlEncoder>(
                  controlKeyForAction(QStringLiteral("lastcue.fx.select_delta")),
                  false)),
          m_pToggleSelectedControl(std::make_unique<ControlPushButton>(
                  controlKeyForAction(QStringLiteral("lastcue.fx.toggle_selected")))),
          m_pTargetDeck1Control(std::make_unique<ControlPushButton>(
                  controlKeyForAction(QStringLiteral("lastcue.fx.target.deck1")))),
          m_pTargetDeck2Control(std::make_unique<ControlPushButton>(
                  controlKeyForAction(QStringLiteral("lastcue.fx.target.deck2")))),
          m_pTargetMasterControl(std::make_unique<ControlPushButton>(
                  controlKeyForAction(QStringLiteral("lastcue.fx.target.master")))),
          m_pTimeAdjustControl(std::make_unique<ControlPotmeter>(
                  controlKeyForAction(QStringLiteral("lastcue.fx.time_adjust")),
                  0.0,
                  1.0,
                  false,
                  false)),
          m_pTimeDeltaControl(std::make_unique<ControlEncoder>(
                  ConfigKey(kFxControlGroup, kTimeDeltaControl),
                  false)),
          m_pTempoRangeDeck1Control(std::make_unique<ControlPushButton>(
                  controlKeyForAction(QStringLiteral(
                          "lastcue.general.deck1.tempo_range")))),
          m_pTempoRangeDeck2Control(std::make_unique<ControlPushButton>(
                  controlKeyForAction(QStringLiteral(
                          "lastcue.general.deck2.tempo_range")))),
          m_pSuppressedControl(std::make_unique<ControlPotmeter>(
                  ConfigKey(kMappingControlGroup, kSuppressedControl),
                  0.0,
                  1.0,
                  false,
                  false)) {
    VERIFY_OR_DEBUG_ASSERT(m_pManager && m_pConfig) {
        return;
    }
    m_captureTimer.setInterval(250);
    m_captureTimer.setSingleShot(true);
    connect(&m_captureTimer,
            &QTimer::timeout,
            this,
            &QmlFxMappingLearning::finishCandidateCapture);
    connect(m_pManager,
            &ControllerManager::devicesChanged,
            this,
            &QmlFxMappingLearning::refreshController);
    connect(m_pManager,
            &ControllerManager::mappingApplied,
            this,
            &QmlFxMappingLearning::refreshController);
    refreshController();
}

QmlFxMappingLearning::~QmlFxMappingLearning() {
    if (m_sessionActive) {
        emit requestClearTemporaryInputMappings();
        emit requestStopLearning();
    }
    disconnectController();
}

QmlFxMappingLearning* QmlFxMappingLearning::create(
        QQmlEngine* pQmlEngine,
        QJSEngine* pJsEngine) {
    Q_UNUSED(pJsEngine);
    VERIFY_OR_DEBUG_ASSERT(s_pControllerManager && s_pUserSettings) {
        qWarning() << "Mapping learning dependencies have not been registered";
        return nullptr;
    }
    return new QmlFxMappingLearning(
            s_pControllerManager, s_pUserSettings, pQmlEngine);
}

void QmlFxMappingLearning::registerDependencies(
        ControllerManager* pManager,
        UserSettingsPointer pConfig) {
    s_pControllerManager = pManager;
    s_pUserSettings = std::move(pConfig);
}

QStringList QmlFxMappingLearning::supportedActionIds() {
    auto result = generalActionIds();
    for (const auto& actionId : singleSelectedActionIds()) {
        if (!result.contains(actionId)) {
            result.append(actionId);
        }
    }
    for (const auto& actionId : group3ActionIds()) {
        if (!result.contains(actionId)) {
            result.append(actionId);
        }
    }
    return result;
}

QStringList QmlFxMappingLearning::generalActionIds() {
    QStringList result;
    result.reserve(kGeneralActionControls.size());
    for (const auto& action : kGeneralActionControls) {
        result.append(action.first);
    }
    return result;
}

QStringList QmlFxMappingLearning::singleSelectedActionIds() {
    QStringList result;
    result.reserve(kSingleSelectedActionControls.size());
    for (const auto& action : kSingleSelectedActionControls) {
        result.append(action.first);
    }
    return result;
}

QStringList QmlFxMappingLearning::group3ActionIds() {
    QStringList result;
    result.reserve(13);
    for (int deck = 1; deck <= 2; ++deck) {
        for (int slot = 1; slot <= 3; ++slot) {
            const auto prefix = QStringLiteral("lastcue.fx.group.deck%1.slot%2.")
                                        .arg(deck)
                                        .arg(slot);
            result.append(prefix + QStringLiteral("toggle"));
            result.append(prefix + QStringLiteral("adjust"));
        }
    }
    result.append(QStringLiteral("lastcue.fx.time_adjust"));
    return result;
}

ConfigKey QmlFxMappingLearning::controlKeyForAction(const QString& actionId) {
    for (const auto& action : kGeneralActionControls) {
        if (action.first == actionId) {
            return action.second;
        }
    }
    for (const auto& action : kSingleSelectedActionControls) {
        if (action.first == actionId) {
            return ConfigKey(kFxControlGroup, action.second);
        }
    }

    static const QRegularExpression groupActionPattern(
            QStringLiteral("^lastcue\\.fx\\.group\\.deck([12])\\.slot([123])\\.(toggle|adjust)$"));
    const auto match = groupActionPattern.match(actionId);
    if (match.hasMatch()) {
        const QString group =
                QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]")
                        .arg(match.captured(1), match.captured(2));
        return ConfigKey(group,
                match.captured(3) == QStringLiteral("toggle")
                        ? QStringLiteral("enabled")
                        : QStringLiteral("meta"));
    }
    return {};
}

QString QmlFxMappingLearning::actionIdForControlKey(const ConfigKey& key) {
    for (const auto& action : kGeneralActionControls) {
        if (action.second == key) {
            return action.first;
        }
    }
    for (const auto& action : kSingleSelectedActionControls) {
        if (ConfigKey(kFxControlGroup, action.second) == key) {
            return action.first;
        }
    }
    if (key == ConfigKey(kFxControlGroup, kTimeDeltaControl)) {
        return QStringLiteral("lastcue.fx.time_adjust");
    }

    static const QRegularExpression groupPattern(
            QStringLiteral("^\\[EffectRack1_EffectUnit([12])_Effect([123])\\]$"));
    const auto match = groupPattern.match(key.group);
    if (match.hasMatch() &&
            (key.item == QStringLiteral("enabled") ||
                    key.item == QStringLiteral("meta"))) {
        return QStringLiteral("lastcue.fx.group.deck%1.slot%2.%3")
                .arg(match.captured(1),
                        match.captured(2),
                        key.item == QStringLiteral("enabled")
                                ? QStringLiteral("toggle")
                                : QStringLiteral("adjust"));
    }
    return {};
}

QString QmlFxMappingLearning::actionIdForMapping(
        const MidiInputMapping& mapping) {
    const auto* pKey = std::get_if<ConfigKey>(&mapping.control);
    if (!pKey) {
        return {};
    }

    const QString directAction = actionIdForControlKey(*pKey);
    if (!directAction.isEmpty()) {
        return directAction;
    }
    if (!mapping.options.testFlag(MidiOption::Script)) {
        return {};
    }

    if (*pKey == ConfigKey(QStringLiteral("[Library]"), QStringLiteral("PioneerDDJSX.backButton"))) {
        return QStringLiteral("lastcue.general.back");
    }
    if (*pKey == ConfigKey(QStringLiteral("[Library]"), QStringLiteral("PioneerDDJSX.rotarySelector"))) {
        return QStringLiteral("lastcue.general.browse.turn");
    }
    if (*pKey == ConfigKey(QStringLiteral("[Library]"), QStringLiteral("PioneerDDJSX.rotarySelectorClick"))) {
        return QStringLiteral("lastcue.general.browse.press");
    }
    int deck = 0;
    if (pKey->group == QStringLiteral("[Channel1]")) {
        deck = 1;
    } else if (pKey->group == QStringLiteral("[Channel2]")) {
        deck = 2;
    }
    if (deck == 0) {
        return {};
    }

    const auto deckAction = [deck](const QString& suffix) {
        return QStringLiteral("lastcue.general.deck%1.%2").arg(deck).arg(suffix);
    };
    if (pKey->item == QStringLiteral("PioneerDDJSX.loadButton")) {
        return QStringLiteral("lastcue.general.load.deck%1").arg(deck);
    }
    if (pKey->item == QStringLiteral("PioneerDDJSX.syncButton")) {
        return deckAction(QStringLiteral("sync"));
    }
    if (pKey->item == QStringLiteral("PioneerDDJSX.shiftKeyLockButton")) {
        return deckAction(QStringLiteral("tempo_range"));
    }
    if (pKey->item == QStringLiteral("PioneerDDJSX.keyLockButton")) {
        return deckAction(QStringLiteral("keylock"));
    }
    if (pKey->item == QStringLiteral("PioneerDDJSX.parameterLeft") &&
            mapping.key.control == 0x24) {
        return deckAction(QStringLiteral("beatjump.backward"));
    }
    if (pKey->item == QStringLiteral("PioneerDDJSX.parameterRight") &&
            mapping.key.control == 0x2C) {
        return deckAction(QStringLiteral("beatjump.forward"));
    }
    return {};
}

ConfigKey QmlFxMappingLearning::controlKeyForTimeBinding(MidiOptions options) {
    return ConfigKey(kFxControlGroup,
            isRelativeOptions(options) ? kTimeDeltaControl : kTimeAdjustControl);
}

QString QmlFxMappingLearning::behaviorKindForOptions(MidiOptions options) {
    if (options.testFlag(MidiOption::FourteenBitMSB) ||
            options.testFlag(MidiOption::FourteenBitLSB)) {
        return QStringLiteral("high-resolution");
    }
    if (isRelativeOptions(options)) {
        return QStringLiteral("relative");
    }
    if (options.testFlag(MidiOption::Button) ||
            options.testFlag(MidiOption::Switch)) {
        return QStringLiteral("button");
    }
    return QStringLiteral("absolute");
}

QString QmlFxMappingLearning::factoryPresetDisplayName(
        const QString& mappingName) {
    return mappingName == QStringLiteral("Pioneer DDJ-SX3 Experimental")
            ? QStringLiteral("Pioneer DDJ-SX3")
            : mappingName;
}

bool QmlFxMappingLearning::isKnownNativeGroup3Controller(
        const QString& controllerName,
        const QString& mappingName) {
    const QString identity = (controllerName + QChar(' ') + mappingName).toLower();
    static const QRegularExpression ddjSxFamily(
            QStringLiteral("\\bddj[ -]sx(?:2|3)?\\b"));
    return ddjSxFamily.match(identity).hasMatch();
}

QString QmlFxMappingLearning::topologyForMappingDescription(
        const QString& description) {
    static const QRegularExpression marker(
            QStringLiteral("LastCUE Box (?:FX-LEARN-01|MAP-01)[^\\n]*topology=(group3|single-selected)\\b"));
    const auto match = marker.match(description);
    return match.hasMatch() ? match.captured(1) : QString();
}

QVariantMap QmlFxMappingLearning::describeLogicalBinding(
        const MidiInputMappings& mappings) {
    QVariantMap result;
    if (mappings.isEmpty()) {
        return result;
    }

    const MidiInputMapping* pMsb = nullptr;
    const MidiInputMapping* pLsb = nullptr;
    for (const auto& mapping : mappings) {
        if (mapping.options.testFlag(MidiOption::FourteenBitMSB)) {
            pMsb = &mapping;
        }
        if (mapping.options.testFlag(MidiOption::FourteenBitLSB)) {
            pLsb = &mapping;
        }
    }

    const bool is14Bit = pMsb && pLsb;
    const auto& first = mappings.first();
    QString behavior = behaviorKindForOptions(first.options);
    const auto opCode = MidiUtils::opCodeFromStatus(first.key.status);
    if (!is14Bit &&
            (opCode == MidiOpCode::NoteOn ||
                    opCode == MidiOpCode::NoteOff)) {
        behavior = QStringLiteral("button");
    }

    result.insert(QStringLiteral("is14Bit"), is14Bit);
    result.insert(QStringLiteral("isRelative"), behavior == QStringLiteral("relative"));
    result.insert(QStringLiteral("behaviorKind"), behavior);
    result.insert(QStringLiteral("status"), static_cast<int>(first.key.status));
    result.insert(QStringLiteral("control"), static_cast<int>(first.key.control));

    QString addressLabel;
    if (is14Bit) {
        result.insert(
                QStringLiteral("msbControl"), static_cast<int>(pMsb->key.control));
        result.insert(
                QStringLiteral("lsbControl"), static_cast<int>(pLsb->key.control));
        addressLabel = QStringLiteral("CH %1 · CC %2+%3")
                               .arg(MidiUtils::channelFromStatus(pMsb->key.status) + 1)
                               .arg(pMsb->key.control)
                               .arg(pLsb->key.control);
        result.insert(QStringLiteral("logicalLabel"),
                QStringLiteral("HIGH RES KNOB · ") + addressLabel);
    } else {
        addressLabel = QStringLiteral("CH %1 · %2/%3")
                               .arg(MidiUtils::channelFromStatus(first.key.status) + 1)
                               .arg(hexByte(first.key.status), hexByte(first.key.control));
        result.insert(QStringLiteral("logicalLabel"), addressLabel);
    }
    result.insert(QStringLiteral("addressLabel"), addressLabel);

    for (const auto& mapping : mappings) {
        if (!mapping.description.trimmed().isEmpty()) {
            result.insert(QStringLiteral("physicalLabel"),
                    mapping.description.trimmed());
            break;
        }
    }
    return result;
}

QString QmlFxMappingLearning::currentActionId() const {
    if (m_currentActionIndex < 0 ||
            m_currentActionIndex >= m_actionIds.size()) {
        return {};
    }
    return m_actionIds.at(m_currentActionIndex);
}

MidiInputMappings QmlFxMappingLearning::mappingsForAction(
        const QMultiHash<uint16_t, MidiInputMapping>& mappings,
        const QString& actionId) const {
    MidiInputMappings result;
    for (const auto& mapping : mappings) {
        if (actionIdForMapping(mapping) == actionId) {
            result.append(mapping);
        }
    }
    return result;
}

QVariantList QmlFxMappingLearning::describeBindings(
        const MidiInputMappings& mappings) const {
    QVariantList result;
    MidiInputMappings highResolution;
    QMap<uint16_t, MidiInputMappings> logicalGroups;
    for (const auto& mapping : mappings) {
        if (mapping.options.testFlag(MidiOption::FourteenBitMSB) ||
                mapping.options.testFlag(MidiOption::FourteenBitLSB)) {
            highResolution.append(mapping);
        } else {
            logicalGroups[normalizedPhysicalKey(mapping)].append(mapping);
        }
    }

    if (highResolution.size() == 2) {
        result.append(describeLogicalBinding(highResolution));
    } else {
        for (const auto& mapping : highResolution) {
            result.append(describeLogicalBinding({mapping}));
        }
    }
    for (const auto& group : logicalGroups) {
        result.append(describeLogicalBinding(group));
    }
    return result;
}

QVariantList QmlFxMappingLearning::currentBindings() const {
    if (!m_pWorkingMapping) {
        return {};
    }
    return describeBindings(mappingsForAction(
            m_pWorkingMapping->getInputMappings(), currentActionId()));
}

QVariantList QmlFxMappingLearning::actionSummaries() const {
    QVariantList result;
    if (!m_pWorkingMapping) {
        return result;
    }

    const auto& workingMappings = m_pWorkingMapping->getInputMappings();
    for (int index = 0; index < m_actionIds.size(); ++index) {
        const QString actionId = m_actionIds.at(index);
        const QVariantList bindings = describeBindings(
                mappingsForAction(workingMappings, actionId));
        QString status;
        if (m_modifiedActionIds.contains(actionId)) {
            status = m_actionStatuses.value(actionId, QStringLiteral("new"));
        } else if (!bindings.isEmpty()) {
            status = QStringLiteral("inherited");
        } else {
            status = QStringLiteral("unmapped");
        }

        QVariantMap summary;
        summary.insert(QStringLiteral("actionId"), actionId);
        summary.insert(QStringLiteral("index"), index);
        summary.insert(QStringLiteral("status"), status);
        summary.insert(QStringLiteral("modified"),
                m_modifiedActionIds.contains(actionId));
        summary.insert(QStringLiteral("pending"), false);
        summary.insert(QStringLiteral("bindings"), bindings);
        summary.insert(QStringLiteral("bindingCount"), bindings.size());
        summary.insert(QStringLiteral("multiple"), bindings.size() > 1);
        result.append(summary);
    }
    return result;
}
bool QmlFxMappingLearning::canSave() const {
    return m_sessionActive && m_state == kStateReview && hasChanges();
}

QVariantList QmlFxMappingLearning::factoryPresets() const {
    return presetCatalog(false);
}

QVariantList QmlFxMappingLearning::customPresets() const {
    return presetCatalog(true);
}

void QmlFxMappingLearning::beginGeneralLearn() {
    beginLearnSession(kSessionGeneral, generalActionIds());
}

void QmlFxMappingLearning::beginFxLearn(const QVariantList& actionIds) {
    QStringList requestedActions;
    requestedActions.reserve(actionIds.size());
    for (const auto& actionId : actionIds) {
        requestedActions.append(actionId.toString());
    }
    if (requestedActions != singleSelectedActionIds() &&
            requestedActions != group3ActionIds()) {
        setErrorMessage(QStringLiteral("Unsupported FX learning action list."));
        return;
    }
    beginLearnSession(kSessionFx, requestedActions);
}

void QmlFxMappingLearning::beginLearnSession(
        const QString& sessionKind, const QStringList& actionIds) {
    if (m_sessionActive) {
        cancelFxLearn();
    }
    refreshController();
    if (!m_available || !m_pController) {
        setErrorMessage(QStringLiteral(
                "Connect exactly one open MIDI controller with a mapping."));
        return;
    }

    auto pCurrentMapping =
            std::dynamic_pointer_cast<LegacyMidiControllerMapping>(
                    m_pController->getMapping());
    if (!pCurrentMapping) {
        setErrorMessage(QStringLiteral(
                "The open MIDI controller has no editable mapping."));
        return;
    }

    m_pWorkingMapping =
            std::make_shared<LegacyMidiControllerMapping>(*pCurrentMapping);
    m_originalInputMappings = pCurrentMapping->getInputMappings();
    m_acceptedBindings.clear();
    m_actionStatuses.clear();
    m_modifiedActionIds.clear();
    m_suppressedMappings.clear();
    m_completedActionIds.clear();
    m_actionIds = actionIds;
    m_currentActionIndex = 0;
    m_sessionKind = sessionKind;
    m_sessionActive = true;
    m_candidateInherited = false;
    m_replaceCurrentActionOnAccept = false;
    m_savedMappingPath.clear();
    emit savedMappingPathChanged();
    emit sessionChanged();
    emit progressChanged();
    emit currentActionChanged();
    emit bindingsChanged();
    emit requestStartLearning();
    showCurrentActionState();
}

void QmlFxMappingLearning::setCurrentActionIndex(int index) {
    if (!m_sessionActive || index < 0 || index >= m_actionIds.size() ||
            (index == m_currentActionIndex && m_state != kStateReview)) {
        return;
    }
    m_captureTimer.stop();
    if (index != m_currentActionIndex) {
        m_currentActionIndex = index;
        emit currentActionChanged();
    }
    emit bindingsChanged();
    showCurrentActionState();
}

void QmlFxMappingLearning::enterReview() {
    if (!m_sessionActive) {
        return;
    }
    m_captureTimer.stop();
    clearCandidate();
    m_replaceCurrentActionOnAccept = false;
    setState(kStateReview);
    emit bindingsChanged();
    emit progressChanged();
}

void QmlFxMappingLearning::addMidiForCurrentAction() {
    if (!m_sessionActive || m_state == kStateReview) {
        return;
    }
    if (m_state == kStateCandidate && !m_candidateInherited &&
            !m_candidateMappings.isEmpty()) {
        m_stagedCandidateMappings.append(m_candidateMappings);
        startCapture(true);
        return;
    }
    m_replaceCurrentActionOnAccept = false;
    startCapture();
}

void QmlFxMappingLearning::remapCurrentAction() {
    if (!m_sessionActive || m_state == kStateReview) {
        return;
    }
    m_replaceCurrentActionOnAccept = true;
    startCapture();
}

void QmlFxMappingLearning::finishCurrentAction(bool replaceExistingMapping) {
    if (!m_sessionActive ||
            (m_state != kStateCandidate && m_state != kStateSuggested)) {
        return;
    }
    acceptCandidate(replaceExistingMapping, false);
}
void QmlFxMappingLearning::acceptCandidate(
        bool replaceExistingMapping,
        bool addAnother) {
    if (!m_sessionActive ||
            (m_state != kStateCandidate && m_state != kStateSuggested) ||
            candidateMappingsForConfirmation().isEmpty()) {
        return;
    }

    if (m_candidateInherited) {
        if (addAnother) {
            m_candidateInherited = false;
            m_replaceCurrentActionOnAccept = false;
            startCapture();
        } else {
            enterReview();
        }
        return;
    }

    if (m_conflictKind == kConflictBoxAction) {
        setErrorMessage(QStringLiteral(
                "This control is already assigned to another Box action."));
        return;
    }
    if (m_conflictKind == kConflictExistingMapping &&
            !replaceExistingMapping) {
        setErrorMessage(QStringLiteral(
                "Confirm Use for Box before replacing its existing function."));
        return;
    }

    const QString actionId = currentActionId();
    const MidiInputMappings confirmedMappings =
            candidateMappingsForConfirmation();
    const auto originalActionMappings = mappingsForAction(
            m_originalInputMappings, actionId);
    const bool replacedOriginalAction =
            m_replaceCurrentActionOnAccept && !originalActionMappings.isEmpty();
    if (m_replaceCurrentActionOnAccept) {
        if (replacedOriginalAction) {
            suppressMappings(originalActionMappings);
        }
        removeWorkingMappingsForAction(actionId);
        removeAcceptedBindingsForAction(actionId);
    }

    if (replaceExistingMapping) {
        for (const auto& candidate : confirmedMappings) {
            const auto existingMappings =
                    m_pWorkingMapping->getInputMappings().values(candidate.key.key);
            if (!existingMappings.isEmpty()) {
                suppressMappings(existingMappings);
            }
            m_pWorkingMapping->removeInputMapping(candidate.key.key);
        }
    }

    for (const auto& mapping : confirmedMappings) {
        m_pWorkingMapping->addInputMapping(mapping.key.key, mapping);
    }

    AcceptedBinding accepted;
    accepted.actionId = actionId;
    accepted.mappings = confirmedMappings;
    accepted.description = m_candidateBinding;
    accepted.physicalKeys = physicalKeysForMappings(m_candidateMappings);
    m_acceptedBindings.append(std::move(accepted));

    m_modifiedActionIds.insert(actionId);
    if (replacedOriginalAction) {
        m_actionStatuses.insert(actionId, QStringLiteral("replaced"));
    } else {
        m_actionStatuses.insert(actionId, QStringLiteral("new"));
    }
    m_candidateInherited = false;
    m_replaceCurrentActionOnAccept = false;
    if (!m_completedActionIds.contains(actionId)) {
        m_completedActionIds.append(actionId);
    }
    rebuildTemporaryMappings();
    emit bindingsChanged();
    emit progressChanged();

    if (addAnother) {
        startCapture();
        return;
    }
    clearCandidate();
    enterReview();
}

void QmlFxMappingLearning::retryCandidate() {
    if (!m_sessionActive || m_state == kStateReview) {
        return;
    }
    startCapture(!m_stagedCandidateMappings.isEmpty());
}

void QmlFxMappingLearning::keepExistingMapping() {
    if (!m_sessionActive || m_state != kStateCandidate ||
            m_conflictKind != kConflictExistingMapping) {
        return;
    }
    advanceAction();
}

void QmlFxMappingLearning::skipCurrentAction() {
    if (!m_sessionActive || m_state == kStateReview) {
        return;
    }
    advanceAction();
}

void QmlFxMappingLearning::cancelFxLearn() {
    if (m_sessionActive) {
        emit requestClearTemporaryInputMappings();
        emit requestStopLearning();
    }
    m_captureTimer.stop();
    m_sessionActive = false;
    m_pWorkingMapping.reset();
    m_originalInputMappings.clear();
    m_acceptedBindings.clear();
    m_actionStatuses.clear();
    m_modifiedActionIds.clear();
    m_suppressedMappings.clear();
    m_completedActionIds.clear();
    m_actionIds.clear();
    m_currentActionIndex = -1;
    m_sessionKind.clear();
    m_candidateInherited = false;
    m_replaceCurrentActionOnAccept = false;
    clearCandidate();
    setErrorMessage({});
    setState(kStateIdle);
    emit sessionChanged();
    emit progressChanged();
    emit currentActionChanged();
    emit bindingsChanged();
}

void QmlFxMappingLearning::saveDerivedFxMapping() {
    if (!canSave() || !m_pWorkingMapping || !m_pController) {
        setErrorMessage(QStringLiteral(
                "Review at least one changed action before saving."));
        return;
    }
    QString mappingName;
    const QString mappingPath = derivedMappingPath(&mappingName);
    if (mappingPath.isEmpty()) {
        setErrorMessage(QStringLiteral(
                "Unable to create the isolated user mapping folder."));
        return;
    }

    m_pWorkingMapping->setName(mappingName);
    QString description = m_pWorkingMapping->description();
    static const QRegularExpression previousMarker(
            QStringLiteral("\\s*LastCUE Box (?:FX-LEARN-01|MAP-01)"
                           "(?: kind=(?:general|fx))?"
                           "(?: topology=(?:group3|single-selected))?"
                           " derived mapping\\."));
    description.remove(previousMarker);
    QString marker = QStringLiteral(" LastCUE Box MAP-01 kind=%1")
                             .arg(m_sessionKind);
    if (m_sessionKind == kSessionFx) {
        const QString topology = m_actionIds == group3ActionIds()
                ? kTopologyGroup3
                : kTopologySingleSelected;
        marker += QStringLiteral(" topology=%1").arg(topology);
    }
    marker += QStringLiteral(" derived mapping.");
    m_pWorkingMapping->setDescription(description + marker);
    m_pWorkingMapping->setFilePath(mappingPath);

    const QString temporaryPath = mappingPath + QStringLiteral(".tmp");
    QFile::remove(temporaryPath);
    if (!m_pWorkingMapping->saveMapping(temporaryPath)) {
        QFile::remove(temporaryPath);
        setErrorMessage(QStringLiteral("Failed to save the derived mapping."));
        return;
    }
    if (QFileInfo::exists(mappingPath) ||
            !QFile::rename(temporaryPath, mappingPath)) {
        QFile::remove(temporaryPath);
        setErrorMessage(QStringLiteral(
                "The derived mapping name is no longer available."));
        return;
    }
    emit presetCatalogChanged();
    m_pWorkingMapping->setDirty(false);

    emit requestClearTemporaryInputMappings();
    emit requestStopLearning();
    m_captureTimer.stop();
    m_sessionActive = false;
    emit sessionChanged();

    auto pSavedMapping = m_pWorkingMapping;
    auto* pController = m_pController.data();
    auto* pManager = m_pManager;
    const auto connectionType =
            m_pManager->thread() == QThread::currentThread()
            ? Qt::DirectConnection
            : Qt::BlockingQueuedConnection;
    const bool applied = QMetaObject::invokeMethod(
            pManager,
            [pManager, pController, pSavedMapping]() {
                pManager->slotApplyMapping(
                        pController, pSavedMapping, true);
            },
            connectionType);
    if (!applied) {
        setErrorMessage(QStringLiteral(
                "Derived mapping was saved but could not be applied."));
        return;
    }
    if (!m_pConfig->save()) {
        setErrorMessage(QStringLiteral(
                "Derived mapping is active but the controller selection could not be persisted."));
        return;
    }

    m_savedMappingPath = mappingPath;
    emit savedMappingPathChanged();
    m_pWorkingMapping.reset();
    m_originalInputMappings.clear();
    m_acceptedBindings.clear();
    m_actionStatuses.clear();
    m_modifiedActionIds.clear();
    m_suppressedMappings.clear();
    m_completedActionIds.clear();
    m_actionIds.clear();
    m_currentActionIndex = -1;
    m_sessionKind.clear();
    setErrorMessage({});
    setState(kStateSaved);
    emit currentActionChanged();
    emit bindingsChanged();
    emit progressChanged();
    emit presetCatalogChanged();
    qInfo() << "LastCUE derived mapping saved and selected" << mappingPath;
}

void QmlFxMappingLearning::applyPreset(const QString& path) {
    if (!isAllowedPresetPath(path)) {
        setErrorMessage(QStringLiteral("This mapping preset is not available."));
        return;
    }
    if (m_sessionActive) {
        cancelFxLearn();
    }
    refreshController();
    if (!m_available || !m_pController) {
        setErrorMessage(QStringLiteral(
                "Connect exactly one open MIDI controller before selecting a preset."));
        return;
    }

    auto pMapping = std::dynamic_pointer_cast<LegacyMidiControllerMapping>(
            LegacyControllerMappingFileHandler::loadMapping(
                    QFileInfo(path), QDir(resourceMappingsPath(m_pConfig))));
    if (!pMapping) {
        setErrorMessage(QStringLiteral("Unable to load the selected preset."));
        return;
    }
    pMapping->setDirty(false);

    auto* pController = m_pController.data();
    auto* pManager = m_pManager;
    const auto connectionType =
            m_pManager->thread() == QThread::currentThread()
            ? Qt::DirectConnection
            : Qt::BlockingQueuedConnection;
    const bool applied = QMetaObject::invokeMethod(
            pManager,
            [pManager, pController, pMapping]() {
                pManager->slotApplyMapping(pController, pMapping, true);
            },
            connectionType);
    if (!applied) {
        setErrorMessage(QStringLiteral("The selected preset could not be applied."));
        return;
    }
    if (!m_pConfig->save()) {
        setErrorMessage(QStringLiteral(
                "The preset is active but the selection could not be persisted."));
        return;
    }
    setErrorMessage({});
    refreshTopologyRecommendation();
    emit presetCatalogChanged();
}


void QmlFxMappingLearning::refreshController() {
    MidiController* pNextController = nullptr;
    int matchingControllers = 0;
    if (m_pManager) {
        const auto controllers = m_pManager->getControllers();
        for (Controller* pController : controllers) {
            auto* pMidiController = qobject_cast<MidiController*>(pController);
            if (pMidiController && pMidiController->isOpen() &&
                    pMidiController->isInputDevice() &&
                    pMidiController->isMappable()) {
                pNextController = pMidiController;
                ++matchingControllers;
            }
        }
    }
    if (matchingControllers != 1) {
        pNextController = nullptr;
    }

    if (pNextController != m_pController) {
        if (m_sessionActive) {
            cancelFxLearn();
        }
        disconnectController();
        connectController(pNextController);
    }

    const bool availableNow = m_pController != nullptr;
    if (m_available != availableNow) {
        m_available = availableNow;
        emit availabilityChanged();
    }
    refreshTopologyRecommendation();
    emit presetCatalogChanged();
}

void QmlFxMappingLearning::refreshTopologyRecommendation() {
    QString controllerLabel;
    QString mappingLabel;
    QString configuredTopology;
    bool nativeGroup3Available = false;
    if (m_pController) {
        controllerLabel = m_pController->getName();
        const auto pMapping = std::dynamic_pointer_cast<LegacyMidiControllerMapping>(
                m_pController->getMapping());
        if (pMapping) {
            mappingLabel = pMapping->name();
            configuredTopology = topologyForMappingDescription(
                    pMapping->description());
            nativeGroup3Available = hasDirectGroup3Mappings(
                    pMapping->getInputMappings());
        }
        nativeGroup3Available = nativeGroup3Available ||
                isKnownNativeGroup3Controller(controllerLabel, mappingLabel);
    }
    const QString recommendedTopology = nativeGroup3Available
            ? kTopologyGroup3
            : kTopologySingleSelected;
    if (m_controllerLabel == controllerLabel &&
            m_mappingLabel == mappingLabel &&
            m_configuredTopology == configuredTopology &&
            m_nativeGroup3Available == nativeGroup3Available &&
            m_recommendedTopology == recommendedTopology) {
        return;
    }
    m_controllerLabel = controllerLabel;
    m_mappingLabel = mappingLabel;
    m_configuredTopology = configuredTopology;
    m_nativeGroup3Available = nativeGroup3Available;
    m_recommendedTopology = recommendedTopology;
    emit topologyRecommendationChanged();
}

// static
bool QmlFxMappingLearning::hasDirectGroup3Mappings(
        const QMultiHash<uint16_t, MidiInputMapping>& mappings) {
    QSet<ConfigKey> required;
    for (int unit = 1; unit <= 2; ++unit) {
        for (int slot = 1; slot <= 3; ++slot) {
            const QString group =
                    QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]")
                            .arg(unit)
                            .arg(slot);
            required.insert(ConfigKey(group, QStringLiteral("enabled")));
            required.insert(ConfigKey(group, QStringLiteral("meta")));
        }
    }
    for (const auto& mapping : mappings) {
        if (const auto* pControl = std::get_if<ConfigKey>(&mapping.control)) {
            required.remove(*pControl);
        }
    }
    return required.isEmpty();
}

void QmlFxMappingLearning::handleMidiMessage(
        unsigned char status,
        unsigned char control,
        unsigned char value) {
    if (!m_sessionActive || m_state != kStateListening) {
        return;
    }
    const MidiKey key(status, control);
    if (MidiUtils::isClockSignal(key)) {
        return;
    }
    m_capturedMessages.append({key, value});
    m_captureTimer.start();
}

void QmlFxMappingLearning::finishCandidateCapture() {
    if (!m_sessionActive || m_capturedMessages.isEmpty()) {
        return;
    }
    m_candidateMappings = LearningUtils::guessMidiInputMappings(
            controlKeyForAction(currentActionId()), m_capturedMessages);
    if (m_candidateMappings.isEmpty()) {
        setErrorMessage(QStringLiteral(
                "Unable to detect one controller input. Try again."));
        m_capturedMessages.clear();
        return;
    }
    if (currentActionId() == QStringLiteral("lastcue.fx.time_adjust")) {
        for (auto& mapping : m_candidateMappings) {
            mapping.control = controlKeyForTimeBinding(mapping.options);
        }
    }

    m_candidateBinding = describeLogicalBinding(m_candidateMappings);
    m_candidateBinding.insert(QStringLiteral("source"), QStringLiteral("learned"));
    m_candidateBinding.insert(QStringLiteral("confidence"), QStringLiteral("captured"));
    m_candidateInherited = false;
    detectCandidateConflicts();
    emit candidateChanged();
    setState(kStateCandidate);
}

void QmlFxMappingLearning::connectController(
        MidiController* pController) {
    m_pController = pController;
    if (!m_pController) {
        return;
    }
    connect(m_pController,
            &MidiController::messageReceived,
            this,
            &QmlFxMappingLearning::handleMidiMessage,
            Qt::QueuedConnection);
    connect(m_pController,
            &Controller::openChanged,
            this,
            &QmlFxMappingLearning::refreshController,
            Qt::QueuedConnection);
    connect(this,
            &QmlFxMappingLearning::requestStartLearning,
            m_pController,
            &MidiController::startLearning,
            Qt::QueuedConnection);
    connect(this,
            &QmlFxMappingLearning::requestStopLearning,
            m_pController,
            &MidiController::stopLearning,
            Qt::QueuedConnection);
    connect(this,
            &QmlFxMappingLearning::requestLearnTemporaryInputMappings,
            m_pController,
            &MidiController::learnTemporaryInputMappings,
            Qt::QueuedConnection);
    connect(this,
            &QmlFxMappingLearning::requestClearTemporaryInputMappings,
            m_pController,
            &MidiController::clearTemporaryInputMappings,
            Qt::QueuedConnection);
}

void QmlFxMappingLearning::disconnectController() {
    if (!m_pController) {
        return;
    }
    disconnect(m_pController, nullptr, this, nullptr);
    disconnect(this, nullptr, m_pController, nullptr);
    m_pController.clear();
}

void QmlFxMappingLearning::setState(const QString& state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

void QmlFxMappingLearning::setErrorMessage(const QString& message) {
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void QmlFxMappingLearning::clearCurrentCandidate() {
    m_candidateMappings.clear();
    m_candidateBinding.clear();
    m_capturedMessages.clear();
    m_conflictKind = kConflictNone;
    m_conflictLabel.clear();
    m_candidateInherited = false;
    emit candidateChanged();
}

void QmlFxMappingLearning::clearCandidate() {
    clearCurrentCandidate();
    m_stagedCandidateMappings.clear();
}

bool QmlFxMappingLearning::showInheritedSuggestion() {
    if (m_modifiedActionIds.contains(currentActionId())) {
        return false;
    }
    const auto mappings = mappingsForAction(
            m_originalInputMappings, currentActionId());
    if (mappings.isEmpty()) {
        return false;
    }
    m_candidateMappings = mappings;
    m_candidateBinding = describeLogicalBinding(mappings);
    m_candidateBinding.insert(QStringLiteral("source"), QStringLiteral("preset"));
    m_candidateBinding.insert(QStringLiteral("confidence"), QStringLiteral("high"));
    m_candidateInherited = true;
    m_conflictKind = kConflictNone;
    m_conflictLabel.clear();
    emit candidateChanged();
    setState(kStateSuggested);
    return true;
}

void QmlFxMappingLearning::showCurrentActionState() {
    if (!m_sessionActive || currentActionId().isEmpty()) {
        return;
    }
    m_captureTimer.stop();
    clearCandidate();
    m_replaceCurrentActionOnAccept = false;
    setErrorMessage({});
    if (showInheritedSuggestion()) {
        return;
    }
    if (!mappingsForAction(
                m_pWorkingMapping->getInputMappings(), currentActionId())
                        .isEmpty()) {
        setState(kStateReady);
        return;
    }
    startCapture();
}


void QmlFxMappingLearning::startCapture(bool preserveStagedCandidates) {
    m_captureTimer.stop();
    if (preserveStagedCandidates) {
        clearCurrentCandidate();
    } else {
        clearCandidate();
    }
    setErrorMessage({});
    setState(kStateListening);
}

void QmlFxMappingLearning::advanceAction() {
    if (!m_sessionActive) {
        return;
    }
    m_captureTimer.stop();
    clearCandidate();
    m_replaceCurrentActionOnAccept = false;
    if (m_currentActionIndex + 1 >= m_actionIds.size()) {
        enterReview();
        return;
    }
    ++m_currentActionIndex;
    emit currentActionChanged();
    emit bindingsChanged();
    showCurrentActionState();
}

void QmlFxMappingLearning::rebuildTemporaryMappings() {
    emit requestClearTemporaryInputMappings();
    MidiInputMappings mappings = m_suppressedMappings;
    for (const auto& binding : std::as_const(m_acceptedBindings)) {
        mappings.append(binding.mappings);
    }
    if (!mappings.isEmpty()) {
        emit requestLearnTemporaryInputMappings(mappings);
    }
}

MidiInputMappings QmlFxMappingLearning::candidateMappingsForConfirmation() const {
    MidiInputMappings mappings = m_stagedCandidateMappings;
    mappings.append(m_candidateMappings);
    return mappings;
}
void QmlFxMappingLearning::detectCandidateConflicts() {
    const MidiInputMappings candidates = candidateMappingsForConfirmation();
    QString conflictingAction;
    if (conflictsWithAcceptedBinding(
                candidates, &conflictingAction) ||
            conflictsWithWorkingBoxAction(
                    candidates, &conflictingAction)) {
        m_conflictKind = kConflictBoxAction;
        m_conflictLabel = conflictingAction;
        return;
    }

    const auto existing = existingMappingLabel(candidates);
    if (!existing.isEmpty()) {
        m_conflictKind = kConflictExistingMapping;
        m_conflictLabel = existing;
        return;
    }
    m_conflictKind = kConflictNone;
    m_conflictLabel.clear();
}

QString QmlFxMappingLearning::existingMappingLabel(
        const MidiInputMappings& mappings) const {
    const auto candidateKeys = physicalKeysForMappings(mappings);
    QSet<QString> labels;
    for (const auto& existing : m_originalInputMappings) {
        if (!candidateKeys.contains(normalizedPhysicalKey(existing))) {
            continue;
        }
        if (actionIdForMapping(existing) == currentActionId()) {
            continue;
        }
        if (const auto* pKey = std::get_if<ConfigKey>(&existing.control)) {
            if (!existing.description.trimmed().isEmpty()) {
                labels.insert(existing.description.trimmed());
            } else {
                labels.insert(QStringLiteral("Existing controller function"));
            }
        } else {
            labels.insert(QStringLiteral("Existing controller behavior"));
        }
    }
    QStringList sortedLabels(labels.cbegin(), labels.cend());
    std::sort(sortedLabels.begin(), sortedLabels.end());
    return sortedLabels.join(QStringLiteral(", "));
}

bool QmlFxMappingLearning::conflictsWithAcceptedBinding(
        const MidiInputMappings& mappings,
        QString* pActionId) const {
    const auto candidateKeys = physicalKeysForMappings(mappings);
    for (const auto& accepted : m_acceptedBindings) {
        if (accepted.actionId == currentActionId()) {
            continue;
        }
        for (uint16_t key : candidateKeys) {
            if (accepted.physicalKeys.contains(key)) {
                if (pActionId) {
                    *pActionId = accepted.actionId;
                }
                return true;
            }
        }
    }
    return false;
}

bool QmlFxMappingLearning::conflictsWithWorkingBoxAction(
        const MidiInputMappings& mappings,
        QString* pActionId) const {
    if (!m_pWorkingMapping) {
        return false;
    }
    const auto candidateKeys = physicalKeysForMappings(mappings);
    for (const auto& existing : m_pWorkingMapping->getInputMappings()) {
        if (!candidateKeys.contains(normalizedPhysicalKey(existing))) {
            continue;
        }
        const auto* pKey = std::get_if<ConfigKey>(&existing.control);
        if (!pKey) {
            continue;
        }
        const QString actionId = actionIdForControlKey(*pKey);
        if (!actionId.isEmpty() && actionId != currentActionId()) {
            if (pActionId) {
                *pActionId = actionId;
            }
            return true;
        }
    }
    return false;
}

void QmlFxMappingLearning::removeWorkingMappingsForAction(
        const QString& actionId) {
    if (!m_pWorkingMapping) {
        return;
    }
    const auto mappings = mappingsForAction(
            m_pWorkingMapping->getInputMappings(), actionId);
    for (const auto& mapping : mappings) {
        m_pWorkingMapping->removeInputMapping(mapping.key.key, mapping);
    }
}

void QmlFxMappingLearning::removeAcceptedBindingsForAction(
        const QString& actionId) {
    for (auto it = m_acceptedBindings.begin();
            it != m_acceptedBindings.end();) {
        if (it->actionId == actionId) {
            it = m_acceptedBindings.erase(it);
        } else {
            ++it;
        }
    }
}

void QmlFxMappingLearning::suppressMappings(
        const MidiInputMappings& mappings) {
    for (const auto& source : mappings) {
        bool alreadySuppressed = false;
        for (const auto& existing : std::as_const(m_suppressedMappings)) {
            if (existing.key.key == source.key.key &&
                    existing.options == source.options) {
                alreadySuppressed = true;
                break;
            }
        }
        if (alreadySuppressed) {
            continue;
        }
        auto suppressed = source;
        suppressed.control = ConfigKey(kMappingControlGroup, kSuppressedControl);
        m_suppressedMappings.append(suppressed);
    }
}


QVariantList QmlFxMappingLearning::presetCatalog(bool custom) const {
    QVariantList result;
    if (!m_pManager) {
        return result;
    }
    auto pEnumerator = custom
            ? m_pManager->getMainThreadUserMappingEnumerator()
            : m_pManager->getMainThreadSystemMappingEnumerator();
    if (!pEnumerator) {
        return result;
    }

    pEnumerator->loadSupportedMappings();
    const auto mappings =
            pEnumerator->getMappingsByExtension(MIDI_MAPPING_EXTENSION);
    const QString currentPath = m_pController && m_pController->getMapping()
            ? normalizedPath(m_pController->getMapping()->filePath())
            : QString();
    QList<QVariantMap> catalog;
    for (const auto& info : mappings) {
        if (!info.isValid()) {
            continue;
        }
        if (custom) {
            if (!info.getDescription().contains(
                        QStringLiteral("LastCUE Box MAP-01")) &&
                    !info.getDescription().contains(
                            QStringLiteral("LastCUE Box FX-LEARN-01"))) {
                continue;
            }
        } else if (!kFactoryPresetNames.contains(info.getName())) {
            continue;
        }

        QVariantMap item;
        item.insert(QStringLiteral("path"), info.getPath());
        item.insert(QStringLiteral("name"), info.getName());
        item.insert(QStringLiteral("displayName"),
                custom ? info.getName()
                       : factoryPresetDisplayName(info.getName()));
        item.insert(QStringLiteral("description"), info.getDescription());
        item.insert(QStringLiteral("custom"), custom);
        item.insert(QStringLiteral("current"),
                normalizedPath(info.getPath()) == currentPath);
        QString provenance = QStringLiteral("lastcue-derived");
        if (!custom) {
            provenance = info.getName() ==
                            QStringLiteral("Pioneer DDJ-SX3 Experimental")
                    ? QStringLiteral("lastcue-adaptation")
                    : QStringLiteral("mixxx-native");
        }
        item.insert(QStringLiteral("provenance"), provenance);
        catalog.append(item);
    }

    std::sort(catalog.begin(), catalog.end(),
            [custom](const QVariantMap& left, const QVariantMap& right) {
                if (custom) {
                    return left.value(QStringLiteral("displayName")).toString()
                                   .localeAwareCompare(
                                           right.value(QStringLiteral("displayName")).toString()) <
                            0;
                }
                return kFactoryPresetNames.indexOf(
                               left.value(QStringLiteral("name")).toString()) <
                        kFactoryPresetNames.indexOf(
                                right.value(QStringLiteral("name")).toString());
            });
    for (const auto& item : catalog) {
        result.append(item);
    }
    return result;
}

bool QmlFxMappingLearning::isAllowedPresetPath(
        const QString& path) const {
    const QString requested = normalizedPath(path);
    const auto isInCatalog = [&requested](const QVariantList& catalog) {
        for (const auto& value : catalog) {
            if (normalizedPath(
                        value.toMap().value(QStringLiteral("path")).toString()) ==
                    requested) {
                return true;
            }
        }
        return false;
    };
    return isInCatalog(factoryPresets()) || isInCatalog(customPresets());
}

QString QmlFxMappingLearning::derivedMappingPath(
        QString* pMappingName) const {
    VERIFY_OR_DEBUG_ASSERT(m_pWorkingMapping && m_pConfig) {
        return {};
    }
    QDir mappingDir(userMappingsPath(m_pConfig));
    if (!mappingDir.mkpath(QStringLiteral("."))) {
        return {};
    }

    QString baseName = m_pWorkingMapping->name().trimmed();
    if (baseName.isEmpty()) {
        baseName = QFileInfo(m_pWorkingMapping->filePath()).completeBaseName();
    }
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("MIDI Controller");
    }
    if (!baseName.contains(QStringLiteral("LastCUE"), Qt::CaseInsensitive)) {
        baseName += QStringLiteral(" LastCUE");
    }
    baseName.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")),
            QStringLiteral("_"));

    QString mappingName = baseName;
    QString path = mappingDir.filePath(mappingName + MIDI_MAPPING_EXTENSION);
    int suffix = 2;
    while (QFileInfo::exists(path)) {
        mappingName = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
        path = mappingDir.filePath(mappingName + MIDI_MAPPING_EXTENSION);
    }
    if (pMappingName) {
        *pMappingName = mappingName;
    }
    return path;
}

// static
QSet<uint16_t> QmlFxMappingLearning::physicalKeysForMappings(
        const MidiInputMappings& mappings) {
    QSet<uint16_t> keys;
    for (const auto& mapping : mappings) {
        keys.insert(normalizedPhysicalKey(mapping));
    }
    return keys;
}

// static
uint16_t QmlFxMappingLearning::normalizedPhysicalKey(
        const MidiInputMapping& mapping) {
    unsigned char status = mapping.key.status;
    if (MidiUtils::opCodeFromStatus(status) == MidiOpCode::NoteOff) {
        status = MidiUtils::statusFromOpCodeAndChannel(
                MidiOpCode::NoteOn,
                MidiUtils::channelFromStatus(status));
    }
    return MidiKey(status, mapping.key.control).key;
}

} // namespace qml
} // namespace mixxx
