#pragma once
#include <QObject>
#include <QQmlEngine>
#include <QModelIndex>
#include <memory>

#include "qml/qmllibrarysourcemodel.h"
#include "qml/qmllibrarytracklistmodel.h"
#include "util/parented_ptr.h"

class Library;

namespace mixxx {
namespace qml {

class QmlLibraryTrackListModel;

class QmlLibraryProxy : public QObject {
    Q_OBJECT
    Q_PROPERTY(mixxx::qml::QmlLibrarySourceModel* sources READ sources CONSTANT)
    Q_PROPERTY(mixxx::qml::QmlLibraryTrackListModel* model READ model NOTIFY modelChanged)
    Q_PROPERTY(bool mediaReady READ mediaReady NOTIFY mediaStatusChanged)
    Q_PROPERTY(bool mediaScanInProgress READ mediaScanInProgress NOTIFY mediaStatusChanged)
    Q_PROPERTY(int mediaDeviceCount READ mediaDeviceCount NOTIFY mediaStatusChanged)
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
    Q_INVOKABLE bool activateSource(const QModelIndex& sourceIndex);
    Q_INVOKABLE QModelIndex parentSource(const QModelIndex& sourceIndex) const;
    Q_INVOKABLE bool loadTrackToPlayer(
            int row, const QString& group, bool play = false);
    bool mediaReady() const;
    bool mediaScanInProgress() const;
    int mediaDeviceCount() const;

    static QmlLibraryProxy* create(QQmlEngine* pQmlEngine, QJSEngine* pJsEngine);
    static void registerLibrary(std::shared_ptr<Library> pLibrary) {
        s_pLibrary = std::move(pLibrary);
    }

  signals:
    void modelChanged();
    void mediaStatusChanged();

  private:
    static inline std::shared_ptr<Library> s_pLibrary;

    std::shared_ptr<Library> m_pLibrary;
    bool m_sourceActivationProvidedModel{false};

    /// This needs to be a plain pointer because it's used as a `Q_PROPERTY` member variable.
    QmlLibrarySourceModel* m_pSourcesProperty;
    QmlLibraryTrackListModel* m_pModelProperty;
};

} // namespace qml
} // namespace mixxx
