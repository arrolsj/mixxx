#pragma once

#include <QHash>
#include <QList>
#include <QMultiHash>
#include <QPair>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

#include "controllers/midi/midimessage.h"
#include "preferences/usersettings.h"

class ControllerManager;
class ControlEncoder;
class ControlPotmeter;
class ControlPushButton;
class LegacyMidiControllerMapping;
class MidiController;

namespace mixxx {
namespace qml {

class QmlFxMappingLearning : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString sessionKind READ sessionKind NOTIFY sessionChanged)
    Q_PROPERTY(int currentActionIndex READ currentActionIndex NOTIFY currentActionChanged)
    Q_PROPERTY(QString currentActionId READ currentActionId NOTIFY currentActionChanged)
    Q_PROPERTY(QVariantMap candidateBinding READ candidateBinding NOTIFY candidateChanged)
    Q_PROPERTY(bool candidateInherited READ candidateInherited NOTIFY candidateChanged)
    Q_PROPERTY(QString conflictKind READ conflictKind NOTIFY candidateChanged)
    Q_PROPERTY(QString conflictLabel READ conflictLabel NOTIFY candidateChanged)
    Q_PROPERTY(QVariantList currentBindings READ currentBindings NOTIFY bindingsChanged)
    Q_PROPERTY(QVariantList actionSummaries READ actionSummaries NOTIFY bindingsChanged)
    Q_PROPERTY(QStringList completedActionIds READ completedActionIds NOTIFY progressChanged)
    Q_PROPERTY(bool hasChanges READ hasChanges NOTIFY progressChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString savedMappingPath READ savedMappingPath NOTIFY savedMappingPathChanged)
    Q_PROPERTY(QVariantList factoryPresets READ factoryPresets NOTIFY presetCatalogChanged)
    Q_PROPERTY(QVariantList customPresets READ customPresets NOTIFY presetCatalogChanged)
    Q_PROPERTY(QString recommendedTopology READ recommendedTopology NOTIFY topologyRecommendationChanged)
    Q_PROPERTY(QString configuredTopology READ configuredTopology NOTIFY topologyRecommendationChanged)
    Q_PROPERTY(bool nativeGroup3Available READ nativeGroup3Available NOTIFY topologyRecommendationChanged)
    Q_PROPERTY(QString controllerLabel READ controllerLabel NOTIFY topologyRecommendationChanged)
    Q_PROPERTY(QString mappingLabel READ mappingLabel NOTIFY topologyRecommendationChanged)
    QML_NAMED_ELEMENT(FxMappingLearning)
    QML_SINGLETON

  public:
    explicit QmlFxMappingLearning(
            ControllerManager* pManager,
            UserSettingsPointer pConfig,
            QObject* pParent = nullptr);
    ~QmlFxMappingLearning() override;

    static QmlFxMappingLearning* create(
            QQmlEngine* pQmlEngine,
            QJSEngine* pJsEngine);
    static void registerDependencies(
            ControllerManager* pManager,
            UserSettingsPointer pConfig);

    static QStringList supportedActionIds();
    static QStringList generalActionIds();
    static QStringList singleSelectedActionIds();
    static QStringList group3ActionIds();
    static ConfigKey controlKeyForAction(const QString& actionId);
    static QString actionIdForControlKey(const ConfigKey& key);
    static QString actionIdForMapping(const MidiInputMapping& mapping);
    static ConfigKey controlKeyForTimeBinding(MidiOptions options);
    static QString behaviorKindForOptions(MidiOptions options);
    static QString factoryPresetDisplayName(const QString& mappingName);
    static bool isKnownNativeGroup3Controller(
            const QString& controllerName,
            const QString& mappingName);
    static QString topologyForMappingDescription(const QString& description);
    static QVariantMap describeLogicalBinding(
            const MidiInputMappings& mappings);

    bool available() const {
        return m_available;
    }
    bool sessionActive() const {
        return m_sessionActive;
    }
    QString state() const {
        return m_state;
    }
    QString sessionKind() const {
        return m_sessionKind;
    }
    int currentActionIndex() const {
        return m_currentActionIndex;
    }
    QString currentActionId() const;
    QVariantMap candidateBinding() const {
        return m_candidateBinding;
    }
    bool candidateInherited() const {
        return m_candidateInherited;
    }
    QString conflictKind() const {
        return m_conflictKind;
    }
    QString conflictLabel() const {
        return m_conflictLabel;
    }
    QVariantList currentBindings() const;
    QVariantList actionSummaries() const;
    QStringList completedActionIds() const {
        return m_completedActionIds;
    }
    bool hasChanges() const {
        return !m_modifiedActionIds.isEmpty();
    }
    bool canSave() const;
    QString errorMessage() const {
        return m_errorMessage;
    }
    QString savedMappingPath() const {
        return m_savedMappingPath;
    }
    QString recommendedTopology() const {
        return m_recommendedTopology;
    }
    QString configuredTopology() const {
        return m_configuredTopology;
    }
    bool nativeGroup3Available() const {
        return m_nativeGroup3Available;
    }
    QString controllerLabel() const {
        return m_controllerLabel;
    }
    QString mappingLabel() const {
        return m_mappingLabel;
    }
    QVariantList factoryPresets() const;
    QVariantList customPresets() const;

    Q_INVOKABLE void beginGeneralLearn();
    Q_INVOKABLE void beginFxLearn(const QVariantList& actionIds);
    Q_INVOKABLE void setCurrentActionIndex(int index);
    Q_INVOKABLE void enterReview();
    Q_INVOKABLE void addMidiForCurrentAction();
    Q_INVOKABLE void remapCurrentAction();
    Q_INVOKABLE void finishCurrentAction(bool replaceExistingMapping = false);
    Q_INVOKABLE void acceptCandidate(
            bool replaceExistingMapping,
            bool addAnother);
    Q_INVOKABLE void retryCandidate();
    Q_INVOKABLE void keepExistingMapping();
    Q_INVOKABLE void skipCurrentAction();
    Q_INVOKABLE void cancelFxLearn();
    Q_INVOKABLE void saveDerivedFxMapping();
    Q_INVOKABLE void applyPreset(const QString& path);

  signals:
    void availabilityChanged();
    void sessionChanged();
    void stateChanged();
    void currentActionChanged();
    void candidateChanged();
    void bindingsChanged();
    void progressChanged();
    void errorMessageChanged();
    void savedMappingPathChanged();
    void topologyRecommendationChanged();
    void presetCatalogChanged();

    void requestStartLearning();
    void requestStopLearning();
    void requestLearnTemporaryInputMappings(const MidiInputMappings& mappings);
    void requestClearTemporaryInputMappings();

  private slots:
    void refreshController();
    void handleMidiMessage(
            unsigned char status,
            unsigned char control,
            unsigned char value);
    void finishCandidateCapture();

  private:
    struct AcceptedBinding {
        QString actionId;
        MidiInputMappings mappings;
        QVariantMap description;
        QSet<uint16_t> physicalKeys;
    };

    void beginLearnSession(
            const QString& sessionKind, const QStringList& actionIds);
    void connectController(MidiController* pController);
    void disconnectController();
    void setState(const QString& state);
    void setErrorMessage(const QString& message);
    void clearCurrentCandidate();
    void clearCandidate();
    bool showInheritedSuggestion();
    void showCurrentActionState();
    void startCapture(bool preserveStagedCandidates = false);
    void advanceAction();
    void rebuildTemporaryMappings();
    void detectCandidateConflicts();
    void refreshTopologyRecommendation();
    static bool hasDirectGroup3Mappings(
            const QMultiHash<uint16_t, MidiInputMapping>& mappings);
    QString existingMappingLabel(const MidiInputMappings& mappings) const;
    bool conflictsWithAcceptedBinding(
            const MidiInputMappings& mappings,
            QString* pActionId) const;
    bool conflictsWithWorkingBoxAction(
            const MidiInputMappings& mappings,
            QString* pActionId) const;
    MidiInputMappings mappingsForAction(
            const QMultiHash<uint16_t, MidiInputMapping>& mappings,
            const QString& actionId) const;
    QVariantList describeBindings(const MidiInputMappings& mappings) const;
    MidiInputMappings candidateMappingsForConfirmation() const;
    void removeWorkingMappingsForAction(const QString& actionId);
    void removeAcceptedBindingsForAction(const QString& actionId);
    void suppressMappings(const MidiInputMappings& mappings);
    QVariantList presetCatalog(bool custom) const;
    bool isAllowedPresetPath(const QString& path) const;
    QString derivedMappingPath(QString* pMappingName) const;
    static QSet<uint16_t> physicalKeysForMappings(
            const MidiInputMappings& mappings);
    static uint16_t normalizedPhysicalKey(const MidiInputMapping& mapping);

    static inline ControllerManager* s_pControllerManager = nullptr;
    static inline UserSettingsPointer s_pUserSettings;

    ControllerManager* const m_pManager;
    UserSettingsPointer m_pConfig;
    QPointer<MidiController> m_pController;
    std::shared_ptr<LegacyMidiControllerMapping> m_pWorkingMapping;
    QMultiHash<uint16_t, MidiInputMapping> m_originalInputMappings;
    QList<AcceptedBinding> m_acceptedBindings;
    QHash<QString, QString> m_actionStatuses;
    QSet<QString> m_modifiedActionIds;
    MidiInputMappings m_suppressedMappings;
    QList<QPair<MidiKey, unsigned char>> m_capturedMessages;
    MidiInputMappings m_candidateMappings;
    MidiInputMappings m_stagedCandidateMappings;
    QStringList m_actionIds;
    QStringList m_completedActionIds;
    int m_currentActionIndex;
    bool m_available;
    bool m_sessionActive;
    bool m_candidateInherited;
    bool m_replaceCurrentActionOnAccept;
    QString m_sessionKind;
    QString m_state;
    QVariantMap m_candidateBinding;
    QString m_conflictKind;
    QString m_conflictLabel;
    QString m_errorMessage;
    QString m_savedMappingPath;
    QString m_recommendedTopology;
    QString m_configuredTopology;
    bool m_nativeGroup3Available;
    QString m_controllerLabel;
    QString m_mappingLabel;
    QTimer m_captureTimer;

    std::unique_ptr<ControlEncoder> m_pSelectDeltaControl;
    std::unique_ptr<ControlPushButton> m_pToggleSelectedControl;
    std::unique_ptr<ControlPushButton> m_pTargetDeck1Control;
    std::unique_ptr<ControlPushButton> m_pTargetDeck2Control;
    std::unique_ptr<ControlPushButton> m_pTargetMasterControl;
    std::unique_ptr<ControlPotmeter> m_pTimeAdjustControl;
    std::unique_ptr<ControlEncoder> m_pTimeDeltaControl;
    std::unique_ptr<ControlPushButton> m_pTempoRangeDeck1Control;
    std::unique_ptr<ControlPushButton> m_pTempoRangeDeck2Control;
    std::unique_ptr<ControlPotmeter> m_pSuppressedControl;
};

} // namespace qml
} // namespace mixxx
