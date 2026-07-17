#pragma once
#include <QIdentityProxyModel>
#include <QQmlEngine>

class QAbstractItemModel;
class TrackModel;

namespace mixxx {
namespace qml {

class QmlLibraryTrackListModel : public QIdentityProxyModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(LibraryTrackListModel)
    QML_UNCREATABLE("Only accessible via Mixxx.Library.model")

  public:
    enum Roles {
        TitleRole = Qt::UserRole,
        ArtistRole,
        AlbumRole,
        AlbumArtistRole,
        BpmRole,
        KeyRole,
        DurationRole,
        FileUrlRole,
    };
    Q_ENUM(Roles);

    QmlLibraryTrackListModel(QAbstractItemModel* pModel, QObject* pParent = nullptr);
    ~QmlLibraryTrackListModel() = default;

    QVariant data(const QModelIndex& index, int role) const override;
    int columnCount(const QModelIndex& index = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QVariant get(int row) const;
    bool setTrackModel(QAbstractItemModel* pModel);

  private:
    TrackModel* m_pTrackModel;
};

} // namespace qml
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::qml::QmlLibraryTrackListModel*)
