#include "qml/qmlplayermanagerproxy.h"

#include <QQmlEngine>

#include "mixer/playermanager.h"
#include "moc_qmlplayermanagerproxy.cpp"
#include "qml/qmlplayerproxy.h"

namespace mixxx {
namespace qml {

QmlPlayerManagerProxy::QmlPlayerManagerProxy(
        std::shared_ptr<PlayerManager> pPlayerManager, QObject* parent)
        : QObject(parent), m_pPlayerManager(pPlayerManager) {
}

QObject* QmlPlayerManagerProxy::getPlayer(const QString& group) {

    BaseTrackPlayer* pPlayer = m_pPlayerManager->getPlayer(group);
    if (!pPlayer) {
        qWarning() << "PlayerManagerProxy failed to find player for group" << group;
        return nullptr;
    }

    QmlPlayerProxy* pCachedProxy = m_playerProxies.value(group, nullptr);
    if (pCachedProxy && pCachedProxy->internalTrackPlayer() == pPlayer) {
        return pCachedProxy;
    }

    // A deck player can be replaced while Mixxx initializes its topology.
    // Retain stale children until this manager is destroyed so existing QML
    // references remain valid, while later calls resolve the current player.
    QmlPlayerProxy* pPlayerProxy = new QmlPlayerProxy(pPlayer, this);
    QQmlEngine::setObjectOwnership(pPlayerProxy, QQmlEngine::CppOwnership);
    connect(pPlayerProxy,
            &QmlPlayerProxy::loadTrackFromLocationRequested,
            this,
            [this, group](const QString& trackLocation, bool play) {
                loadLocationToPlayer(trackLocation, group, play);
            });
    connect(pPlayerProxy,
            &QmlPlayerProxy::cloneFromGroup,
            this,
            [this, group](const QString& sourceGroup) {
                m_pPlayerManager->slotCloneDeck(sourceGroup, group);
            });
    m_playerProxies.insert(group, pPlayerProxy);
    return pPlayerProxy;
}

void QmlPlayerManagerProxy::loadLocationIntoNextAvailableDeck(
        const QString& trackLocation, bool play) {
    m_pPlayerManager->slotLoadLocationIntoNextAvailableDeck(trackLocation, play);
}

void QmlPlayerManagerProxy::loadLocationUrlIntoNextAvailableDeck(
        const QUrl& trackLocationUrl, bool play) {
    if (trackLocationUrl.isLocalFile()) {
        loadLocationIntoNextAvailableDeck(trackLocationUrl.toLocalFile(), play);
    } else {
        qWarning() << "QmlPlayerManagerProxy: URL" << trackLocationUrl << "is not a local file!";
    }
}

void QmlPlayerManagerProxy::loadLocationToPlayer(
        const QString& location, const QString& group, bool play) {
    m_pPlayerManager->slotLoadLocationToPlayer(location, group, play);
}

// static
QmlPlayerManagerProxy* QmlPlayerManagerProxy::create(QQmlEngine* pQmlEngine, QJSEngine* pJsEngine) {
    // The implementation of this method is mostly taken from the code example
    // that shows the replacement for `qmlRegisterSingletonInstance()` when
    // using `QML_SINGLETON`.
    // https://doc.qt.io/qt-6/qqmlengine.html#QML_SINGLETON

    // The instance has to exist before it is used. We cannot replace it.
    VERIFY_OR_DEBUG_ASSERT(s_pPlayerManager) {
        qWarning() << "PlayerManager hasn't been registered yet";
        return nullptr;
    }
    return new QmlPlayerManagerProxy(s_pPlayerManager, pQmlEngine);
}

} // namespace qml
} // namespace mixxx
