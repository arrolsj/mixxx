#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#ifdef MIXXX_USE_QML
#include "qml/qmlplayermanagerproxy.h"
#include "qml/qmlplayerproxy.h"
#endif
#include "sources/soundsourceproxy.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "util/cmdlineargs.h"

namespace {

const QString kTrackLocationTest1 = QStringLiteral("id3-test-data/cover-test-png.mp3");
const QString kTrackLocationTest2 = QStringLiteral("id3-test-data/cover-test-vbr.mp3");

void deleteTrack(Track* pTrack) {
    // Delete track objects directly in unit tests with
    // no main event loop
    delete pTrack;
};

void waitForTrackToBeLoaded(Deck* pDeck) {
    while (!pDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded()) {
        QTest::qSleep(100); // millis
    }
}

bool waitForTrackToBeReleased(
        Deck* pDeck,
        EngineMixer* pEngine,
        int timeoutMillis = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!pDeck->isTrackReleased() &&
            !elapsed.hasExpired(timeoutMillis)) {
        pEngine->process(1024);
        QTest::qWait(10);
    }
    return pDeck->isTrackReleased();
}

} // namespace

// We can't inherit from LibraryTest because that creates a key_notation control object that is also
// created by the Library object itself. The duplicated CO creation causes a debug assert.
class PlayerManagerTest : public MixxxDbTest, SoundSourceProviderRegistration {
  public:
    PlayerManagerTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        // This setup mirrors coreservices -- it would be nice if we could use coreservices instead
        // but it does a lot of local disk / settings setup.
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(m_pConfig, pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(
                m_pConfig,
                "[Master]",
                m_pEffectsManager.get(),
                pChannelHandleFactory,
                true);
        m_pSoundManager = std::make_shared<SoundManager>(m_pConfig, m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(m_pSoundManager.get());

        CoverArtCache::createInstance();

        m_pPlayerManager = std::make_shared<PlayerManager>(m_pConfig,
                m_pSoundManager.get(),
                m_pEffectsManager.get(),
                m_pEngine.get());

        m_pPlayerManager->addConfiguredDecks();
        m_pPlayerManager->addSampler();
        PlayerInfo::create();
        m_pEffectsManager->setup();

        const auto dbConnection = mixxx::DbConnectionPooled(dbConnectionPooler());
        if (!MixxxDb::initDatabaseSchema(dbConnection)) {
            exit(1);
        }
        m_pTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr,
                m_pConfig,
                dbConnectionPooler(),
                deleteTrack);

        m_pRecordingManager = std::make_shared<RecordingManager>(m_pConfig, m_pEngine.get());
        m_pLibrary = std::make_shared<Library>(
                nullptr,
                m_pConfig,
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());

        m_pPlayerManager->bindToLibrary(m_pLibrary.get());
    }

    ~PlayerManagerTest() {
        m_pSoundManager.reset();
        m_pPlayerManager.reset();
        PlayerInfo::destroy();
        m_pLibrary.reset();
        m_pRecordingManager.reset();
        m_pEngine.reset();
        m_pEffectsManager.reset();
        m_pTrackCollectionManager.reset();
        m_pControlIndicatorTimer.reset();
        CoverArtCache::destroy();
    }

  protected:
    TrackPointer getOrAddTrackByLocation(
            const QString& trackLocation) const {
        return m_pTrackCollectionManager->getOrAddTrack(
                TrackRef::fromFilePath(trackLocation));
    }

    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::unique_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::shared_ptr<Library> m_pLibrary;
};

class PlayerManagerSafeStopTest : public PlayerManagerTest {
  public:
    void SetUp() override {
        m_previousModeWasSet =
                qEnvironmentVariableIsSet("LASTCUE_REMOVABLE_MEDIA_MODE");
        m_previousRootsWereSet =
                qEnvironmentVariableIsSet("LASTCUE_FAKE_MEDIA_ROOTS");
        m_previousMode = qgetenv("LASTCUE_REMOVABLE_MEDIA_MODE");
        m_previousRoots = qgetenv("LASTCUE_FAKE_MEDIA_ROOTS");
        qputenv("LASTCUE_REMOVABLE_MEDIA_MODE", "fake");
        qputenv("LASTCUE_FAKE_MEDIA_ROOTS",
                getTestDir().absolutePath().toLocal8Bit());
        m_pConfig->set(
                ConfigKey("[Library]", "ShowRekordboxLibrary"),
                ConfigValue(1));
        PlayerManagerTest::SetUp();
    }

    void TearDown() override {
        PlayerManagerTest::TearDown();
        restoreEnvironmentVariable("LASTCUE_REMOVABLE_MEDIA_MODE",
                m_previousModeWasSet,
                m_previousMode);
        restoreEnvironmentVariable("LASTCUE_FAKE_MEDIA_ROOTS",
                m_previousRootsWereSet,
                m_previousRoots);
    }

  private:
    static void restoreEnvironmentVariable(
            const char* name,
            bool wasSet,
            const QByteArray& value) {
        if (wasSet) {
            qputenv(name, value);
        } else {
            qunsetenv(name);
        }
    }

    bool m_previousModeWasSet{false};
    bool m_previousRootsWereSet{false};
    QByteArray m_previousMode;
    QByteArray m_previousRoots;
};

#ifdef MIXXX_USE_QML
TEST_F(PlayerManagerTest, QmlPlayerProxyIsManagerOwnedAndSharedPerPlayer) {
    QPointer<QObject> pDeck1Guard;
    {
        mixxx::qml::QmlPlayerManagerProxy playerManagerProxy(m_pPlayerManager);

        QObject* pDeck1Proxy =
                playerManagerProxy.getPlayer(QStringLiteral("[Channel1]"));
        QObject* pDeck1ProxyAgain =
                playerManagerProxy.getPlayer(QStringLiteral("[Channel1]"));
        QObject* pDeck2Proxy =
                playerManagerProxy.getPlayer(QStringLiteral("[Channel2]"));

        ASSERT_NE(pDeck1Proxy, nullptr);
        EXPECT_EQ(pDeck1ProxyAgain, pDeck1Proxy);
        ASSERT_NE(pDeck2Proxy, nullptr);
        EXPECT_NE(pDeck2Proxy, pDeck1Proxy);
        EXPECT_EQ(pDeck1Proxy->parent(), &playerManagerProxy);
        EXPECT_EQ(QQmlEngine::objectOwnership(pDeck1Proxy),
                QQmlEngine::CppOwnership);

        auto* pTypedDeck1Proxy =
                qobject_cast<mixxx::qml::QmlPlayerProxy*>(pDeck1Proxy);
        ASSERT_NE(pTypedDeck1Proxy, nullptr);
        EXPECT_EQ(pTypedDeck1Proxy->internalTrackPlayer(),
                m_pPlayerManager->getPlayer(QStringLiteral("[Channel1]")));
        pDeck1Guard = pDeck1Proxy;
    }
    EXPECT_TRUE(pDeck1Guard.isNull());
}
#endif

TEST_F(PlayerManagerTest, TrackLoadsWithoutEngineCallback) {
    auto deck1 = m_pPlayerManager->getDeck(0);
    TrackPointer pTrack =
            getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack);

    deck1->slotLoadTrack(pTrack, false);

    // A missing audio device means EngineMixer::process() is not called. The
    // track-loading worker must still run so the UI receives confirmation.
    QTRY_VERIFY_WITH_TIMEOUT(
            deck1->getEngineDeck()->getEngineBuffer()->isTrackLoaded(),
            5000);
    EXPECT_EQ(pTrack, deck1->getLoadedTrack());
}

TEST_F(PlayerManagerTest, UnEjectTest) {
    // Ejecting an empty deck with no previously-recorded ejected track has no effect.
    auto deck1 = m_pPlayerManager->getDeck(0);
    deck1->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck1->getLoadedTrack());

    // Load a track and eject it
    TrackPointer pTrack1 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    TrackId testId1 = pTrack1->getId();
    ASSERT_TRUE(testId1.isValid());
    deck1->slotLoadTrack(pTrack1, false);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    // make sure eject does not trigger 'unreplace':
    // sleep for longer than 500 ms 'unreplace' period so this is not registered as double-click
    QTest::qSleep(kUnreplaceDelay); // millis
    deck1->slotEjectTrack(1.0);

    // Load another track.
    TrackPointer pTrack2 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2, false);

    // Ejecting in an empty deck loads the last-ejected track.
    auto deck2 = m_pPlayerManager->getDeck(1);
    ASSERT_EQ(nullptr, deck2->getLoadedTrack());
    // make sure eject does not trigger 'unreplace'
    QTest::qSleep(kUnreplaceDelay); // millis
    deck2->slotEjectTrack(2.0);
    ASSERT_NE(nullptr, deck2->getLoadedTrack());
    ASSERT_EQ(testId1, deck2->getLoadedTrack()->getId());
}

TEST_F(PlayerManagerTest, StopAndEjectTrackClearsPausedAndPlayingDeck) {
    auto deck1 = m_pPlayerManager->getDeck(0);
    const ConfigKey playKey(deck1->getGroup(), QStringLiteral("play"));

    TrackPointer pTrack1 =
            getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    deck1->slotLoadTrack(pTrack1, false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_FALSE(ControlObject::toBool(playKey));

    deck1->stopAndEjectTrack();
    EXPECT_EQ(nullptr, deck1->getLoadedTrack());
    EXPECT_FALSE(deck1->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    EXPECT_FALSE(ControlObject::toBool(playKey));
    EXPECT_TRUE(waitForTrackToBeReleased(
            deck1, m_pEngine.get()));

    TrackPointer pTrack2 =
            getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2, true);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_TRUE(ControlObject::toBool(playKey));

    deck1->stopAndEjectTrack();
    EXPECT_EQ(nullptr, deck1->getLoadedTrack());
    EXPECT_FALSE(deck1->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    EXPECT_FALSE(ControlObject::toBool(playKey));
    EXPECT_TRUE(waitForTrackToBeReleased(
            deck1, m_pEngine.get()));
}

TEST_F(PlayerManagerSafeStopTest,
        WaitsForReaderReleaseBeforePlatformSafeStop) {
    QElapsedTimer elapsed;
    elapsed.start();
    while ((m_pLibrary->isRekordboxDeviceScanRunning() ||
                   m_pLibrary->removableMediaDevices().isEmpty()) &&
            !elapsed.hasExpired(5000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(10);
    }
    ASSERT_FALSE(m_pLibrary->isRekordboxDeviceScanRunning());
    const auto devices = m_pLibrary->removableMediaDevices();
    ASSERT_EQ(1, devices.size());
    ASSERT_TRUE(devices.constFirst().canStop);

    auto deck1 = m_pPlayerManager->getDeck(0);
    TrackPointer pTrack =
            getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack);
    deck1->slotLoadTrack(pTrack, true);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_FALSE(deck1->isTrackReleased());

    qRegisterMetaType<mixxx::removablemedia::SafeStopResult>();
    QSignalSpy safeStopSpy(
            m_pLibrary.get(),
            &Library::removableMediaSafeStopFinished);
    ASSERT_TRUE(m_pLibrary->requestRemovableMediaSafeStop(
            devices.constFirst().deviceId,
            devices.constFirst().generation));
    EXPECT_EQ(nullptr, deck1->getLoadedTrack());
    EXPECT_FALSE(deck1->isTrackReleased());

    // Event processing alone may run the release poll, but cannot consume the
    // CachingReader worker acknowledgement. The fake platform stop must still
    // be pending until an engine callback observes TRACK_UNLOADED.
    QTest::qWait(50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_TRUE(safeStopSpy.isEmpty());
    ASSERT_EQ(1, m_pLibrary->removableMediaDevices().size());
    EXPECT_EQ(mixxx::removablemedia::MediaState::Quiescing,
            m_pLibrary->removableMediaDevices().constFirst().state);

    elapsed.restart();
    while (safeStopSpy.isEmpty() && !elapsed.hasExpired(5000)) {
        m_pEngine->process(1024);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(10);
    }
    ASSERT_FALSE(safeStopSpy.isEmpty());
    ASSERT_TRUE(deck1->isTrackReleased());
    const auto result =
            qvariant_cast<mixxx::removablemedia::SafeStopResult>(
                    safeStopSpy.constFirst().constFirst());
    EXPECT_EQ(mixxx::removablemedia::MediaState::SafeToRemove,
            result.state);
    EXPECT_EQ(mixxx::removablemedia::ErrorCode::None,
            result.errorCode);
}

// Loading a new track in a deck causes the old one to be ejected.
// That old track can be unejected into a different deck.
TEST_F(PlayerManagerTest, UnEjectReplaceTrackTest) {
    auto deck1 = m_pPlayerManager->getDeck(0);
    // Load a track and the load another one
    TrackPointer pTrack1 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    TrackId testId1 = pTrack1->getId();
    ASSERT_TRUE(testId1.isValid());
    deck1->slotLoadTrack(pTrack1, false);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);

    // Load another track, replacing the first, causing it to be unloaded.
    TrackPointer pTrack2 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2, false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);

    // Ejecting in an empty deck loads the last-ejected track.
    auto deck2 = m_pPlayerManager->getDeck(1);
    ASSERT_EQ(nullptr, deck2->getLoadedTrack());
    // make sure eject does not trigger 'unreplace'
    QTest::qSleep(kUnreplaceDelay);
    deck2->slotEjectTrack(1.0);
    ASSERT_NE(nullptr, deck2->getLoadedTrack());
    ASSERT_EQ(testId1, deck2->getLoadedTrack()->getId());
}

TEST_F(PlayerManagerTest, UnEjectInvalidTrackIdTest) {
    // Save an invalid trackid in playermanager.
    auto pTrack = Track::newDummy(
            getTestDir().filePath(kTrackLocationTest1), TrackId(QVariant(10)));
    ASSERT_NE(nullptr, pTrack);
    m_pPlayerManager->slotSaveEjectedTrack(pTrack);
    auto deck1 = m_pPlayerManager->getDeck(0);
    // Does nothing -- no crash.
    // make sure eject does not trigger 'unreplace'
    QTest::qSleep(kUnreplaceDelay);
    deck1->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck1->getLoadedTrack());
}

TEST_F(PlayerManagerTest, UnReplaceTest) {
    // Trigger eject twice within 500 ms to undo track replacement
    auto deck1 = m_pPlayerManager->getDeck(0);
    // Load a track
    TrackPointer pTrack1 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    TrackId testId1 = pTrack1->getId();
    ASSERT_TRUE(testId1.isValid());
    deck1->slotLoadTrack(pTrack1, false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    // Load another track.
    TrackPointer pTrack2 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2, false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    // Eject. Make sure eject does not trigger 'unreplace':
    // sleep for longer than 500 ms 'unreplace' period so this is not registered as double-click
    QTest::qSleep(kUnreplaceDelay); // millis
    deck1->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck1->getLoadedTrack());

    // Eject again, assume this is reached faster than 500 ms after first eject
    deck1->slotEjectTrack(1.0);
    // First track should be reloaded
    ASSERT_NE(nullptr, deck1->getLoadedTrack());
    ASSERT_EQ(testId1, deck1->getLoadedTrack()->getId());
}
