// This feature reads tracks, playlists and folders from removable Recordbox
// prepared devices (USB drives, etc), by parsing the binary *.PDB files
// stored on each removable device. It does not read the locally stored
// Rekordbox database (Collection).

// It draws heavily from the hard work completed here:

//      https://github.com/Deep-Symmetry/crate-digger

// And uses the C++ Kaitai Struct binary parsing libraries:

//      http://kaitai.io
//      https://github.com/kaitai-io/kaitai_struct
//      https://github.com/kaitai-io/kaitai_struct_cpp_stl_runtime

// The *.PDB C++ files:

//      rekordbox_pdb.h
//      rekordbox_pdb.cpp

// Were generated from the following structure definition file:

//      https://github.com/Deep-Symmetry/crate-digger/blob/master/src/main/kaitai/rekordbox_pdb.ksy

#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QSet>
#include <QStringListModel>
#include <QtConcurrentRun>
#include <fstream>
#include <functional>

#include "library/baseexternallibraryfeature.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/removablemedia/removablemediaservice.h"
#include "library/treeitemmodel.h"
#include "util/parented_ptr.h"

class TrackCollectionManager;
class BaseExternalPlaylistModel;

struct RekordboxPlaylistTreeEntry {
    int parentIndex{-1};
    QString label;
    QString path;
};

struct RekordboxImportResult {
    QString devicePath;
    QString deviceLabel;
    QString deviceId;
    quint64 generation{0};
    bool succeeded{false};
    QList<RekordboxPlaylistTreeEntry> playlistTree;
};

struct RekordboxDeviceScanResult {
    quint64 generation{0};
    QList<TreeItem*> treeItems;
    QList<mixxx::removablemedia::RemovableMediaDevice> mediaDevices;
};

class RekordboxPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    RekordboxPlaylistModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

  protected:
    void initSortColumnMapping() override;
};

class RekordboxFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    RekordboxFeature(Library* pLibrary,
            UserSettingsPointer pConfig,
            std::unique_ptr<mixxx::removablemedia::IRemovableMediaService>
                    pRemovableMediaService = {});
    ~RekordboxFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;
    int deviceCount() const;
    bool isDeviceScanRunning() const;
    bool isDatabaseImportRunning() const;
    QString deviceScanState() const;
    void scanDevices();
    QList<mixxx::removablemedia::RemovableMediaDevice> mediaDevices() const;
    bool requestSafeStop(
            const QString& deviceId,
            quint64 expectedGeneration,
            const std::function<void()>& releaseMediaReferences = {});
    bool isSafeStopActiveFor(const QString& deviceId) const;
    void rejectSafeStop(const QString& deviceId, const QString& busyReason);
    bool isLocationLoadAllowed(const QString& location) const;
    QString deviceIdForLocation(const QString& location) const;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void refreshLibraryModels();
    void onRekordboxDevicesFound();
    void onTracksFound();

  signals:
    void devicesChanged();
    void deviceScanStateChanged();
    void databaseImportStateChanged();
    void safeStopFinished(
            const mixxx::removablemedia::SafeStopResult& result);
    void activeDeviceRemoved(const QString& deviceId);

  private slots:
    void htmlLinkClicked(const QUrl& link);
    void onRemovableMediaDevicesChanged();
    void onSafeStopFinished(
            const mixxx::removablemedia::SafeStopResult& result);

  private:
    QString formatRootViewHtml() const;
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<RekordboxPlaylistModel> m_pRekordboxPlaylistModel;

    void startDeviceScan(bool replaceCompletedScan = false);
    void setMediaDeviceState(
            const QString& deviceId,
            mixxx::removablemedia::MediaState state,
            mixxx::removablemedia::ErrorCode errorCode =
                    mixxx::removablemedia::ErrorCode::None,
            const QString& errorMessage = {},
            const QString& busyReason = {});
    void makeDeviceImportRetryable(
            const QString& deviceId,
            const QString& devicePath = {},
            const QString& deviceLabel = {});

    QFutureWatcher<RekordboxDeviceScanResult> m_devicesFutureWatcher;
    QFuture<RekordboxDeviceScanResult> m_devicesFuture;
    QFutureWatcher<RekordboxImportResult> m_tracksFutureWatcher;
    QFuture<RekordboxImportResult> m_tracksFuture;
    QString m_title;
    bool m_featureLoadingRequested{false};
    bool m_deviceScanPending{false};
    bool m_deviceScanApplyPending{false};
    bool m_databaseImportApplyPending{false};
    quint64 m_activeScanGeneration{0};
    QString m_activeImportDeviceId;
    quint64 m_activeImportGeneration{0};
    QString m_activeDeviceId;
    QSet<QString> m_loadBlockedDeviceIds;
    QStringList m_safeStopBlockedDeviceIds;
    QStringList m_safeStopBlockedMountPoints;
    QString m_safeStopOperationDeviceId;
    QString m_safeStopRemovalGroupId;
    QList<mixxx::removablemedia::RemovableMediaDevice> m_mediaDevices;

    std::unique_ptr<mixxx::removablemedia::IRemovableMediaService>
            m_pRemovableMediaService;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
