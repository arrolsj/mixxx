#include "qml/qmlfxmappinglearning.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <algorithm>
#include <utility>
#include <variant>

#include "control/controlencoder.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "controllers/controllermanager.h"
#include "controllers/defs_controllers.h"
#include "controllers/learningutils.h"
#include "controllers/midi/legacymidicontrollermapping.h"
#include "controllers/midi/midicontroller.h"
#include "controllers/midi/midiutils.h"
#include "moc_qmlfxmappinglearning.cpp"
#include "util/assert.h"

namespace mixxx {
namespace qml {
namespace {
const QString kControlGroup = QStringLiteral("[LastCueFX]");
const QString kStateIdle = QStringLiteral("idle");
const QString kStateListening = QStringLiteral("listening");
const QString kStateCandidate = QStringLiteral("candidate");
const QString kStateComplete = QStringLiteral("complete");
const QString kStateSaved = QStringLiteral("saved");
const QString kConflictNone = QStringLiteral("none");
const QString kConflictExistingMapping = QStringLiteral("existing-mapping");
const QString kConflictBoxAction = QStringLiteral("box-action");

const QList<QPair<QString, QString>> kSingleSelectedActionControls = {
        {QStringLiteral("lastcue.fx.select_delta"), QStringLiteral("select_delta")},
        {QStringLiteral("lastcue.fx.toggle_selected"), QStringLiteral("toggle_selected")},
        {QStringLiteral("lastcue.fx.target.deck1"), QStringLiteral("target_deck1")},
        {QStringLiteral("lastcue.fx.target.deck2"), QStringLiteral("target_deck2")},
        {QStringLiteral("lastcue.fx.target.master"), QStringLiteral("target_master")},
        {QStringLiteral("lastcue.fx.time_adjust"), QStringLiteral("time_adjust")},
};
const QString kTopologySingleSelected = QStringLiteral("single-selected");
const QString kTopologyGroup3 = QStringLiteral("group3");
const QString kTimeAdjustControl = QStringLiteral("time_adjust");
const QString kTimeDeltaControl = QStringLiteral("time_delta");

QString hexByte(unsigned char value) {
    return QStringLiteral("0x%1")
            .arg(QString::number(value, 16).toUpper().rightJustified(2, QChar('0')));
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
                  ConfigKey(kControlGroup, kTimeDeltaControl),
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

// static
QmlFxMappingLearning* QmlFxMappingLearning::create(
        QQmlEngine* pQmlEngine,
        QJSEngine* pJsEngine) {
    Q_UNUSED(pJsEngine);
    VERIFY_OR_DEBUG_ASSERT(s_pControllerManager && s_pUserSettings) {
        qWarning() << "FX mapping learning dependencies have not been registered";
        return nullptr;
    }
    return new QmlFxMappingLearning(
            s_pControllerManager, s_pUserSettings, pQmlEngine);
}

// static
void QmlFxMappingLearning::registerDependencies(
        ControllerManager* pManager,
        UserSettingsPointer pConfig) {
    s_pControllerManager = pManager;
    s_pUserSettings = std::move(pConfig);
}

// static
QStringList QmlFxMappingLearning::supportedActionIds() {
    auto result = singleSelectedActionIds();
    for (const auto& actionId : group3ActionIds()) {
        if (!result.contains(actionId)) {
            result.append(actionId);
        }
    }
    return result;
}

// static
QStringList QmlFxMappingLearning::singleSelectedActionIds() {
    QStringList result;
    result.reserve(kSingleSelectedActionControls.size());
    for (const auto& action : kSingleSelectedActionControls) {
        result.append(action.first);
    }
    return result;
}

// static
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

// static
ConfigKey QmlFxMappingLearning::controlKeyForAction(const QString& actionId) {
    for (const auto& action : kSingleSelectedActionControls) {
        if (action.first == actionId) {
            return ConfigKey(kControlGroup, action.second);
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

// static
ConfigKey QmlFxMappingLearning::controlKeyForTimeBinding(MidiOptions options) {
    const bool relative = options.testFlag(MidiOption::SelectKnob) ||
            options.testFlag(MidiOption::Spread64);
    return ConfigKey(kControlGroup,
            relative ? kTimeDeltaControl : kTimeAdjustControl);
}

// static
bool QmlFxMappingLearning::isKnownNativeGroup3Controller(
        const QString& controllerName,
        const QString& mappingName) {
    const QString identity = (controllerName + QChar(' ') + mappingName).toLower();
    static const QRegularExpression ddjSxFamily(
            QStringLiteral("\\bddj[ -]sx(?:2|3)?\\b"));
    return ddjSxFamily.match(identity).hasMatch();
}

// static
QString QmlFxMappingLearning::topologyForMappingDescription(
        const QString& description) {
    static const QRegularExpression marker(
            QStringLiteral("LastCUE Box FX-LEARN-01 topology=(group3|single-selected)\\b"));
    const auto match = marker.match(description);
    return match.hasMatch() ? match.captured(1) : QString();
}

// static
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
    result.insert(QStringLiteral("is14Bit"), is14Bit);
    result.insert(QStringLiteral("status"), static_cast<int>(first.key.status));
    result.insert(QStringLiteral("control"), static_cast<int>(first.key.control));
    if (is14Bit) {
        result.insert(
                QStringLiteral("msbControl"), static_cast<int>(pMsb->key.control));
        result.insert(
                QStringLiteral("lsbControl"), static_cast<int>(pLsb->key.control));
        result.insert(
                QStringLiteral("logicalLabel"),
                QStringLiteral("14-BIT CC %1 / %2 CH %3")
                        .arg(pMsb->key.control)
                        .arg(pLsb->key.control)
                        .arg(MidiUtils::channelFromStatus(pMsb->key.status) + 1));
    } else {
        result.insert(
                QStringLiteral("logicalLabel"),
                QStringLiteral("%1 / %2 CH %3")
                        .arg(hexByte(first.key.status))
                        .arg(hexByte(first.key.control))
                        .arg(MidiUtils::channelFromStatus(first.key.status) + 1));
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

QVariantList QmlFxMappingLearning::currentBindings() const {
    QVariantList result;
    const auto actionId = currentActionId();
    for (const auto& binding : m_acceptedBindings) {
        if (binding.actionId == actionId) {
            result.append(binding.description);
        }
    }
    return result;
}

void QmlFxMappingLearning::beginFxLearn(const QVariantList& actionIds) {
    if (m_sessionActive) {
        cancelFxLearn();
    }
    refreshController();
    if (!m_available || !m_pController) {
        setErrorMessage(QStringLiteral(
                "Connect exactly one open MIDI controller with a mapping."));
        return;
    }

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
    m_completedActionIds.clear();
    m_actionIds = requestedActions;
    m_currentActionIndex = 0;
    m_sessionActive = true;
    m_savedMappingPath.clear();
    emit savedMappingPathChanged();
    emit sessionChanged();
    emit progressChanged();
    emit currentActionChanged();
    emit bindingsChanged();
    emit requestStartLearning();
    startCapture();
}

void QmlFxMappingLearning::acceptCandidate(
        bool replaceExistingMapping,
        bool addAnother) {
    if (!m_sessionActive || m_candidateMappings.isEmpty() ||
            m_conflictKind == kConflictBoxAction) {
        return;
    }
    if (m_conflictKind == kConflictExistingMapping &&
            !replaceExistingMapping) {
        setErrorMessage(QStringLiteral(
                "Existing mapping is blocked until Use for Box FX is confirmed."));
        return;
    }

    if (replaceExistingMapping) {
        for (const auto& mapping : std::as_const(m_candidateMappings)) {
            m_pWorkingMapping->removeInputMapping(mapping.key.key);
        }
    }

    for (const auto& mapping : std::as_const(m_candidateMappings)) {
        m_pWorkingMapping->addInputMapping(mapping.key.key, mapping);
    }

    AcceptedBinding accepted;
    accepted.actionId = currentActionId();
    accepted.mappings = m_candidateMappings;
    accepted.description = m_candidateBinding;
    accepted.physicalKeys = physicalKeysForMappings(m_candidateMappings);
    m_acceptedBindings.append(std::move(accepted));
    rebuildTemporaryMappings();
    emit bindingsChanged();

    if (addAnother) {
        startCapture();
        return;
    }

    if (!m_completedActionIds.contains(currentActionId())) {
        m_completedActionIds.append(currentActionId());
        emit progressChanged();
    }
    advanceAction();
}

void QmlFxMappingLearning::retryCandidate() {
    if (!m_sessionActive || m_state == kStateComplete) {
        return;
    }
    startCapture();
}

void QmlFxMappingLearning::keepExistingMapping() {
    if (!m_sessionActive || m_state != kStateCandidate ||
            m_conflictKind != kConflictExistingMapping) {
        return;
    }
    skipCurrentAction();
}

void QmlFxMappingLearning::skipCurrentAction() {
    if (!m_sessionActive || m_state == kStateComplete) {
        return;
    }
    if (!m_completedActionIds.contains(currentActionId())) {
        m_completedActionIds.append(currentActionId());
        emit progressChanged();
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
    m_completedActionIds.clear();
    m_actionIds.clear();
    m_currentActionIndex = -1;
    clearCandidate();
    setErrorMessage({});
    setState(kStateIdle);
    emit sessionChanged();
    emit progressChanged();
    emit currentActionChanged();
    emit bindingsChanged();
}

void QmlFxMappingLearning::saveDerivedFxMapping() {
    if (!m_sessionActive || m_state != kStateComplete ||
            !m_pWorkingMapping || !m_pController) {
        setErrorMessage(QStringLiteral(
                "Complete or skip every FX action before saving."));
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
            QStringLiteral("\\s*LastCUE Box FX-LEARN-01(?: topology=(?:group3|single-selected))? derived mapping\\."));
    description.remove(previousMarker);
    const QString topology = m_actionIds == group3ActionIds()
            ? kTopologyGroup3
            : kTopologySingleSelected;
    m_pWorkingMapping->setDescription(
            description +
            QStringLiteral(" LastCUE Box FX-LEARN-01 topology=%1 derived mapping.")
                    .arg(topology));
    m_pWorkingMapping->setFilePath(mappingPath);
    if (!m_pWorkingMapping->saveMapping(mappingPath)) {
        QFile::remove(mappingPath);
        setErrorMessage(QStringLiteral("Failed to save the derived mapping."));
        return;
    }
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
    setErrorMessage({});
    setState(kStateSaved);
    qInfo() << "LastCUE FX derived mapping saved and selected" << mappingPath;
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
    qInfo() << "LastCUE FX topology recommendation"
            << m_recommendedTopology << "controller" << m_controllerLabel
            << "mapping" << m_mappingLabel
            << "native-group3" << m_nativeGroup3Available;
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
    MidiKey key(status, control);
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
                "Unable to detect one logical MIDI control. Retry."));
        m_capturedMessages.clear();
        return;
    }
    if (currentActionId() == QStringLiteral("lastcue.fx.time_adjust")) {
        for (auto& mapping : m_candidateMappings) {
            mapping.control = controlKeyForTimeBinding(mapping.options);
        }
    }

    m_candidateBinding = describeLogicalBinding(m_candidateMappings);
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

void QmlFxMappingLearning::clearCandidate() {
    m_candidateMappings.clear();
    m_candidateBinding.clear();
    m_capturedMessages.clear();
    m_conflictKind = kConflictNone;
    m_conflictLabel.clear();
    emit candidateChanged();
}

void QmlFxMappingLearning::startCapture() {
    m_captureTimer.stop();
    clearCandidate();
    setErrorMessage({});
    setState(kStateListening);
}

void QmlFxMappingLearning::advanceAction() {
    clearCandidate();
    ++m_currentActionIndex;
    emit currentActionChanged();
    emit bindingsChanged();
    if (m_currentActionIndex >= m_actionIds.size()) {
        setState(kStateComplete);
        return;
    }
    startCapture();
}

void QmlFxMappingLearning::rebuildTemporaryMappings() {
    emit requestClearTemporaryInputMappings();
    MidiInputMappings mappings;
    for (const auto& binding : std::as_const(m_acceptedBindings)) {
        mappings.append(binding.mappings);
    }
    if (!mappings.isEmpty()) {
        emit requestLearnTemporaryInputMappings(mappings);
    }
}

void QmlFxMappingLearning::detectCandidateConflicts() {
    QString conflictingAction;
    if (conflictsWithAcceptedBinding(
                m_candidateMappings, &conflictingAction)) {
        m_conflictKind = kConflictBoxAction;
        m_conflictLabel = conflictingAction;
        return;
    }

    const auto existing = existingMappingLabel(m_candidateMappings);
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
    QSet<QString> labels;
    for (const auto& mapping : mappings) {
        const auto existingMappings =
                m_originalInputMappings.values(mapping.key.key);
        for (const auto& existing : existingMappings) {
            if (const auto* pKey =
                            std::get_if<ConfigKey>(&existing.control)) {
                labels.insert(pKey->group + QStringLiteral(" ") + pKey->item);
            } else {
                labels.insert(QStringLiteral("SCRIPT INPUT"));
            }
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
    baseName += QStringLiteral(" LastCUE FX");
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
