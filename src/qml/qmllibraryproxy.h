#pragma once
#include <QModelIndex>
#include <QObject>
#include <QQmlEngine>
#include <memory>

#include "qml/qmllibrarysourcemodel.h"
#include "qml/qmllibrarytracklistmodel.h"
#include "qml/qmlremovablemediamodel.h"
#include "util/parented_ptr.h"

class Library;

namespace mixxx {
namespace qml {

class QmlLibraryTrackListModel;

class QmlLibraryProxy : public QObject {
    Q_OBJECT
    Q_PROPERTY(mixxx::qml::QmlLibrarySourceModel* sources READ sources CONSTANT)
    Q_PROPERTY(mixxx::qml::QmlLibraryTrackListModel* model READ model NOTIFY modelChanged)
    Q_PROPERTY(mixxx::qml::QmlRemovableMediaModel* mediaDevices READ mediaDevices CONSTANT)
    Q_PROPERTY(bool mediaReady READ mediaReady NOTIFY mediaStatusChanged)
    Q_PROPERTY(bool mediaScanInProgress READ mediaScanInProgress NOTIFY mediaStatusChanged)
    Q_PROPERTY(bool mediaImportInProgress READ mediaImportInProgress NOTIFY mediaStatusChanged)
    Q_PROPERTY(int mediaDeviceCount READ mediaDeviceCount NOTIFY mediaStatusChanged)
    Q_PROPERTY(QString mediaScanState READ mediaScanState NOTIFY mediaStatusChanged)
    QML_NAMED_ELEMENT(Library)
    QML_SINGLETON

  public:
    explicit QmlLibraryProxy(std::shared_ptr<Library> pLibrary, QObject* parent = nullptr);

    QmlLibrarySourceModel* sources() const {
        return m_pSourcesProperty;
    }
    QmlLibraryTrackListModel* model() const {
        return m_pModelProperty;
    }
    QmlRemovableMediaModel* mediaDevices() const {
        return m_pMediaDevicesProperty;
    }
    Q_INVOKABLE bool activateSource(const QModelIndex& sourceIndex);
    Q_INVOKABLE QModelIndex parentSource(const QModelIndex& sourceIndex) const;
    Q_INVOKABLE bool loadTrackToPlayer(
            int row, const QString& group, bool play = false);
    Q_INVOKABLE void requestMediaRescan();
    Q_INVOKABLE bool requestSafeStop(
            const QString& deviceId,
            qulonglong expectedGeneration);
    Q_INVOKABLE QModelIndex sourceIndexForDevice(
            const QString& deviceId) const;
    bool mediaReady() const;
    bool mediaScanInProgress() const;
    bool mediaImportInProgress() const;
    int mediaDeviceCount() const;
    QString mediaScanState() const;

    static QmlLibraryProxy* create(QQmlEngine* pQmlEngine, QJSEngine* pJsEngine);
    static void registerLibrary(std::shared_ptr<Library> pLibrary) {
        s_pLibrary = std::move(pLibrary);
    }

  signals:
    void modelChanged();
    void mediaStatusChanged();
    void mediaSafeStopFinished(
            const QString& deviceId,
            const QString& state,
            const QString& errorCode,
            const QString& errorMessage);
    void mediaActiveDeviceRemoved(const QString& deviceId);

  private:
    static inline std::shared_ptr<Library> s_pLibrary;

    std::shared_ptr<Library> m_pLibrary;
    bool m_sourceActivationProvidedModel{false};

    /// This needs to be a plain pointer because it's used as a `Q_PROPERTY` member variable.
    QmlLibrarySourceModel* m_pSourcesProperty;
    QmlLibraryTrackListModel* m_pModelProperty;
    QmlRemovableMediaModel* m_pMediaDevicesProperty;
};

} // namespace qml
} // namespace mixxx
