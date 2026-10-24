/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findersidebar.h"
#include "finderaccent.h"
#include "finderairdrop.h"
#include "finderairdropservice.h"
#include "findercloud.h"
#include "findercloudsheet.h"

#include "views/draganddrophelper.h"

#include <KFilePlacesModel>
#include <KIO/Global>
#include <KLocalizedString>

#include <QAction>
#include <QContextMenuEvent>
#include <QApplication>
#include <QDesktopServices>
#include <QDrag>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTime>
#include <QTimer>

#include <algorithm>

namespace
{
constexpr int RowHeight = 30;
constexpr int HeaderHeight = 34;
constexpr int SideMargin = 10; // selection inset from the sidebar edges
constexpr int IconSize = 18;

const QColor HeaderColor(255, 255, 255, 110);
const QColor TextColor(255, 255, 255, 225);
const QColor SelectionColor(255, 255, 255, 34);
const QColor HoverColor(255, 255, 255, 14);

// A favorite being dragged within the sidebar: its places-model row.
const QString PlaceMime = QStringLiteral("application/x-finder-sidebar-place");

// Finder's sidebar glyphs, picked from the icon theme's symbolic set.
QString symbolicIconFor(const QUrl &url, const QString &iconName)
{
    static const QHash<QString, QString> byIcon = {
        {QStringLiteral("folder-download"), QStringLiteral("folder-download-symbolic")},
        {QStringLiteral("folder-downloads"), QStringLiteral("folder-download-symbolic")},
        {QStringLiteral("network-workgroup"), QStringLiteral("network-workgroup-symbolic")},
    };
    if (url.scheme() == QLatin1String("recentlyused")) {
        return QStringLiteral("document-open-recent-symbolic");
    }
    if (url.scheme() == QLatin1String("trash")) {
        return QStringLiteral("user-trash-symbolic");
    }
    if (byIcon.contains(iconName)) {
        return byIcon.value(iconName);
    }
    const QString symbolic = iconName + QStringLiteral("-symbolic");
    return QIcon::hasThemeIcon(symbolic) ? symbolic : QStringLiteral("folder-symbolic");
}

// Draws a symbolic icon in a single color.
QPixmap tinted(const QIcon &icon, int size, qreal dpr, const QColor &color)
{
    QPixmap pm = icon.pixmap(QSize(size, size), dpr);
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), color);
    return pm;
}

class SidebarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return QSize(100, index.data(FinderSidebar::HeaderRole).toBool() ? HeaderHeight : RowHeight);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect;
        const bool active = option.widget ? option.widget->isActiveWindow() : bool(option.state & QStyle::State_Active);

        if (index.data(FinderSidebar::HeaderRole).toBool()) {
            QFont f = option.font;
            f.setPointSizeF(f.pointSizeF() * 0.85);
            f.setWeight(QFont::DemiBold);
            p->setFont(f);
            p->setPen(HeaderColor);
            p->drawText(r.adjusted(SideMargin + 8, 0, -SideMargin, -4), Qt::AlignLeft | Qt::AlignBottom, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        const QRectF pill = QRectF(r).adjusted(SideMargin, 1, -SideMargin, -1);
        if (option.state & QStyle::State_Selected) {
            QPainterPath path;
            path.addRoundedRect(pill, 6, 6);
            p->fillPath(path, SelectionColor);
        } else if (option.state & QStyle::State_MouseOver) {
            QPainterPath path;
            path.addRoundedRect(pill, 6, 6);
            p->fillPath(path, HoverColor);
        }

        // Golden Gate: accent-colored sidebar icons while the window is active.
        const QString iconName = index.data(Qt::DecorationRole).toString();
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        const QColor color = active ? finderAccent() : QColor(160, 160, 165);
        // Tinting means a theme lookup and a fresh pixmap per row, and the
        // sidebar repaints 30 times a second while a cloud drive syncs.
        const QString key = iconName + QLatin1Char('|') + QString::number(color.rgba()) + QLatin1Char('|') + QString::number(dpr);
        QPixmap pm = m_tinted.value(key);
        if (pm.isNull()) {
            static const QIcon airDrop = FinderAirDropView::icon(Qt::white);
            const QIcon icon = iconName == QLatin1String("finder-airdrop") ? airDrop : QIcon::fromTheme(iconName);
            pm = tinted(icon, IconSize, dpr, color);
            if (m_tinted.size() > 200) {
                m_tinted.clear(); // icon theme or accent changes leave old entries behind
            }
            m_tinted.insert(key, pm);
        }
        const int iconX = int(pill.left()) + 8;
        p->drawPixmap(QPoint(iconX, r.top() + (r.height() - IconSize) / 2), pm);

        // Cloud drives: a spinner while syncing or connecting, a badge when
        // they need the user.
        int statusWidth = 0;
        if (const FinderCloud *cloud = FinderSidebar::cloudAt(index)) {
            const QRectF badge(pill.right() - 8 - 12, r.top() + (r.height() - 12) / 2.0, 12, 12);
            if (cloud->state() == FinderCloud::SignedOut || cloud->state() == FinderCloud::Failed) {
                p->setPen(Qt::NoPen);
                p->setBrush(QColor(0xff, 0x9f, 0x0a));
                p->drawEllipse(badge);
                p->setPen(QPen(QColor(30, 30, 30), 1.6, Qt::SolidLine, Qt::RoundCap));
                p->drawLine(QPointF(badge.center().x(), badge.top() + 3), QPointF(badge.center().x(), badge.center().y() + 1));
                p->drawPoint(QPointF(badge.center().x(), badge.bottom() - 2.8));
                statusWidth = 20;
            } else if (cloud->isBusy() || cloud->state() == FinderCloud::Starting) {
                const int angle = (QTime::currentTime().msecsSinceStartOfDay() % 1000) * 360 / 1000;
                p->setBrush(Qt::NoBrush);
                p->setPen(QPen(QColor(255, 255, 255, 60), 1.8));
                p->drawEllipse(badge.adjusted(1, 1, -1, -1));
                p->setPen(QPen(QColor(255, 255, 255, 190), 1.8, Qt::SolidLine, Qt::RoundCap));
                p->drawArc(badge.adjusted(1, 1, -1, -1), -angle * 16, 100 * 16);
                statusWidth = 20;
            }
        }

        p->setPen(TextColor);
        QFont font = option.font;
        if (option.state & QStyle::State_Selected) {
            font.setWeight(QFont::DemiBold); // Finder bolds the current place
        }
        p->setFont(font);
        const QRect textRect(iconX + IconSize + 8, r.top(), int(pill.right()) - (iconX + IconSize + 8) - 6 - statusWidth, r.height());
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width()));
        p->restore();
    }

private:
    mutable QHash<QString, QPixmap> m_tinted;
};
}

FinderSidebar::FinderSidebar(KFilePlacesModel *places, QWidget *parent)
    : QListView(parent)
    , m_places(places)
    , m_model(new QStandardItemModel(this))
    , m_spinner(new QTimer(this))
{
    setModel(m_model);
    setItemDelegate(new SidebarDelegate(this));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformItemSizes(false);
    // Transparent: the window paints the glass behind the sidebar.
    setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);
    setStyleSheet(QStringLiteral("QListView { background: transparent; border: none; }"));
    // Drops are handled here, not by QListView: folders pin between
    // favorites, files go into the item they land on.
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(false);

    connect(m_places, &QAbstractItemModel::rowsInserted, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::rowsRemoved, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::rowsMoved, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::dataChanged, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::modelReset, this, &FinderSidebar::rebuild);
    connect(m_places, &KFilePlacesModel::setupDone, this, [this](const QModelIndex &index, bool success) {
        if (success && index == m_pendingSetup) {
            const QUrl url = m_places->url(index);
            m_pendingSetupNewTab ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
        }
        m_pendingSetup = QPersistentModelIndex();
    });

    for (FinderCloud *cloud : FinderCloud::all()) {
        connect(cloud, &FinderCloud::stateChanged, this, [this, cloud](FinderCloud::State state) {
            rebuild();
            if (cloud != m_pendingCloud) {
                return;
            }
            if (state == FinderCloud::Ready) {
                const QUrl url = QUrl::fromLocalFile(cloud->mountPath());
                m_pendingCloudNewTab ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
                m_pendingCloud.clear();
            } else if (state == FinderCloud::SignedOut || state == FinderCloud::Failed || state == FinderCloud::NotConnected) {
                m_pendingCloud.clear();
            }
        });
        connect(cloud, &FinderCloud::activityChanged, this, &FinderSidebar::updateCloud);
    }
    m_spinner->setInterval(33);
    connect(m_spinner, &QTimer::timeout, this, [this]() {
        // Only the spinning rows, not the whole sidebar (and the glass behind it).
        for (int row = 0; row < m_model->rowCount(); ++row) {
            if (cloudAt(m_model->index(row, 0))) {
                viewport()->update(visualRect(m_model->index(row, 0)));
            }
        }
    });
    rebuild();
}

void FinderSidebar::rebuild()
{
    QList<QStandardItem *> top; // Recents, above the first heading
    QList<QStandardItem *> favorites;
    QList<QStandardItem *> locations;
    QStandardItem *trash = nullptr;
    const QList<FinderCloud *> clouds = FinderCloud::all();

    for (int row = 0; row < m_places->rowCount(); ++row) {
        const QModelIndex index = m_places->index(row, 0);
        if (m_places->isHidden(index)) {
            continue;
        }
        const QUrl url = m_places->url(index);
        const auto group = m_places->groupType(index);
        QString label = m_places->text(index);
        if (group == KFilePlacesModel::RecentlySavedType) {
            // Finder has a single "Recents" entry.
            if (url.toString() != QLatin1String("recentlyused:/files")) {
                continue;
            }
            label = i18nc("@item:inlistbox Finder sidebar", "Recents");
        } else if (group == KFilePlacesModel::SearchForType || group == KFilePlacesModel::TagsType) {
            continue;
        } else if (url.isLocalFile() && std::any_of(clouds.cbegin(), clouds.cend(), [&url](const FinderCloud *cloud) {
                       return url.toLocalFile() == cloud->mountPath();
                   })) {
            continue; // a cloud drive's mount (or a bookmark of it); listed below by name
        }

        auto *item = new QStandardItem(label);
        item->setData(symbolicIconFor(url, m_places->icon(index).name()), Qt::DecorationRole);
        item->setData(url, UrlRole);
        item->setData(row, PlaceIndexRole);
        item->setToolTip(url.isLocalFile() ? url.toLocalFile() : url.toDisplayString());

        if (url.scheme() == QLatin1String("trash")) {
            trash = item;
        } else if (group == KFilePlacesModel::RecentlySavedType) {
            top.append(item);
        } else if (group == KFilePlacesModel::PlacesType) {
            item->setData(true, FavoriteRole);
            favorites.append(item);
        } else {
            locations.append(item); // devices, removable media, network
        }
    }
    if (trash) {
        locations.append(trash);
    }

    m_model->clear();
    auto header = [this](const QString &text) {
        auto *item = new QStandardItem(text);
        item->setData(true, HeaderRole);
        item->setFlags(Qt::NoItemFlags);
        m_model->appendRow(item);
    };
    for (auto *item : std::as_const(top)) {
        m_model->appendRow(item);
    }
    if (!favorites.isEmpty()) {
        header(i18nc("@title Finder sidebar section", "Favorites"));
        for (auto *item : std::as_const(favorites)) {
            m_model->appendRow(item);
        }
    }
    auto cloudItem = [](FinderCloud *cloud) {
        auto *item = new QStandardItem(cloud->name());
        item->setData(cloud->symbolicIconName(), Qt::DecorationRole);
        item->setData(QUrl::fromLocalFile(cloud->mountPath()), UrlRole);
        item->setData(int(cloud->provider()), CloudRole);
        return item;
    };
    // iCloud has its own section above Locations, as in Finder. It's
    // always there: before signing in, clicking it opens the sign-in sheet.
    header(i18nc("@title Finder sidebar section", "iCloud"));
    m_model->appendRow(cloudItem(FinderCloud::instance(FinderCloud::ICloudDrive)));
    if (!locations.isEmpty()) {
        header(i18nc("@title Finder sidebar section", "Locations"));
        // AirDrop leads Locations, as in Finder; it opens the AirDrop panel.
        if (m_airDropShown) {
            auto *airDrop = new QStandardItem(i18nc("@item:inlistbox Finder sidebar", "AirDrop"));
            airDrop->setData(QStringLiteral("finder-airdrop"), Qt::DecorationRole);
            airDrop->setData(AirDropUrl(), UrlRole);
            m_model->appendRow(airDrop);
        }
        // Other cloud storage follows AirDrop, as in Finder.
        if (FinderCloud *google = FinderCloud::instance(FinderCloud::GoogleDrive); google->hasAccount()) {
            m_model->appendRow(cloudItem(google));
        }
        for (auto *item : std::as_const(locations)) {
            m_model->appendRow(item);
        }
    }
    setUrl(m_url);
    updateCloud();
}

FinderCloud *FinderSidebar::cloudAt(const QModelIndex &index)
{
    const QVariant provider = index.data(CloudRole);
    return provider.isValid() ? FinderCloud::instance(FinderCloud::Provider(provider.toInt())) : nullptr;
}

void FinderSidebar::updateCloud()
{
    bool spinning = false;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (const FinderCloud *cloud = cloudAt(m_model->index(row, 0))) {
            const QString activity =
                cloud->state() == FinderCloud::NotConnected ? i18nc("@info:tooltip Finder sidebar", "Click to sign in") : cloud->activityText();
            m_model->item(row)->setToolTip(i18nc("@info:tooltip Finder sidebar, %1 is a drive, %2 e.g. Up to date", "%1 — %2", cloud->name(), activity));
            spinning = spinning || cloud->isBusy() || cloud->state() == FinderCloud::Starting;
        }
    }
    if (spinning && !m_spinner->isActive()) {
        m_spinner->start();
    } else if (!spinning) {
        m_spinner->stop();
    }
    viewport()->update();
}

void FinderSidebar::connectCloud(FinderCloud::Provider provider)
{
    FinderCloud *cloud = FinderCloud::instance(provider);
    QDialog *sheet = FinderCloudSheet::create(cloud, window());
    sheet->setAttribute(Qt::WA_DeleteOnClose);
    connect(sheet, &QDialog::accepted, this, [this, cloud]() {
        Q_EMIT placeActivated(QUrl::fromLocalFile(cloud->mountPath()));
    });
    sheet->open();
}

void FinderSidebar::activateCloud(FinderCloud *cloud, bool newTab)
{
    switch (cloud->state()) {
    case FinderCloud::Ready: {
        const QUrl url = QUrl::fromLocalFile(cloud->mountPath());
        newTab ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
        return;
    }
    case FinderCloud::NotConnected:
    case FinderCloud::SignedOut:
        connectCloud(cloud->provider());
        return;
    case FinderCloud::SigningIn:
        return;
    case FinderCloud::Starting:
    case FinderCloud::Failed:
        // Open it as soon as it's mounted.
        m_pendingCloud = cloud;
        m_pendingCloudNewTab = newTab;
        cloud->start();
        return;
    }
}

void FinderSidebar::cloudMenu(QMenu &menu, FinderCloud *cloud)
{
    QAction *status = menu.addAction(cloud->activityText());
    status->setEnabled(false);
    menu.addSeparator();
    if (cloud->state() == FinderCloud::NotConnected) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("im-user-online")), i18nc("@action:inmenu Finder cloud drive, %1 is e.g. iCloud Drive", "Sign In to %1…", cloud->name()), this,
                       [this, cloud]() {
                           connectCloud(cloud->provider());
                       });
        return;
    }
    if (cloud->state() == FinderCloud::SignedOut) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("im-user-online")), i18nc("@action:inmenu Finder cloud drive", "Sign In Again…"), this, [this, cloud]() {
            connectCloud(cloud->provider());
        });
    } else if (cloud->state() == FinderCloud::Failed) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18nc("@action:inmenu Finder cloud drive", "Try Again"), cloud, &FinderCloud::start);
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")),
                   i18nc("@action:inmenu Finder cloud drive, %1 is e.g. iCloud Drive", "Open %1 on the Web", cloud->name()), this, [cloud]() {
                       QDesktopServices::openUrl(cloud->webUrl());
                   });
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("network-disconnect")),
                   i18nc("@action:inmenu Finder cloud drive, %1 is e.g. iCloud Drive", "Disconnect %1…", cloud->name()), this, [this, cloud]() {
                       QString text = i18nc("@info Finder cloud drive, %1 is e.g. iCloud Drive",
                                            "Your files stay in %1. Finder removes the drive from this computer, along with its local copies, "
                                            "and signs this computer out.",
                                            cloud->name());
                       if (cloud->uploads() > 0 || cloud->uploadErrors() > 0) {
                           text = i18ncp("@info Finder cloud drive",
                                         "1 file hasn't finished uploading. If you disconnect now, the changes to it are lost.",
                                         "%1 files haven't finished uploading. If you disconnect now, the changes to them are lost.",
                                         cloud->uploads() + cloud->uploadErrors());
                       }
                       const QString title = i18nc("@title Finder cloud drive, %1 is e.g. iCloud Drive", "Disconnect %1?", cloud->name());
                       QMessageBox box(QMessageBox::Warning, title, title, QMessageBox::Cancel, window());
                       box.setInformativeText(text);
                       QPushButton *disconnect = box.addButton(i18nc("@action:button Finder cloud drive", "Disconnect"), QMessageBox::DestructiveRole);
                       box.setDefaultButton(QMessageBox::Cancel);
                       box.exec();
                       if (box.clickedButton() == disconnect) {
                           cloud->disconnect();
                       }
                   });
}

void FinderSidebar::setAirDropShown(bool shown)
{
    if (m_airDropShown != shown) {
        m_airDropShown = shown;
        rebuild();
    }
}

QUrl FinderSidebar::url() const
{
    return m_url;
}

KFilePlacesModel *FinderSidebar::placesModel() const
{
    return m_places;
}

bool FinderSidebar::isAirDropShown() const
{
    return m_airDropShown;
}

QUrl FinderSidebar::AirDropUrl()
{
    return QUrl(QStringLiteral("finder-airdrop:/"));
}

void FinderSidebar::setUrl(const QUrl &url)
{
    m_url = url;
    // Finder highlights a place only while showing that exact folder, not
    // while inside one of its subfolders.
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex index = m_model->index(row, 0);
        const QUrl place = index.data(UrlRole).toUrl();
        if (place.isValid() && place.matches(url, QUrl::StripTrailingSlash)) {
            selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
            return;
        }
    }
    selectionModel()->clear();
}

QModelIndex FinderSidebar::placeIndex(const QModelIndex &row) const
{
    const QVariant placeRow = row.data(PlaceIndexRole);
    return placeRow.isValid() ? m_places->index(placeRow.toInt(), 0) : QModelIndex();
}

void FinderSidebar::activate(const QModelIndex &index, bool newTab)
{
    if (index.data(UrlRole).toUrl() == AirDropUrl()) {
        Q_EMIT airDropRequested();
        return;
    }
    if (FinderCloud *cloud = cloudAt(index)) {
        activateCloud(cloud, newTab);
        return;
    }
    const QModelIndex place = placeIndex(index);
    if (!place.isValid()) {
        return;
    }
    if (m_places->setupNeeded(place)) {
        // Mount the device first; setupDone opens it.
        m_pendingSetup = place;
        m_pendingSetupNewTab = newTab;
        m_places->requestSetup(place);
        return;
    }
    const QUrl url = m_places->url(place);
    newTab ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
}

void FinderSidebar::mousePressEvent(QMouseEvent *event)
{
    const QModelIndex index = indexAt(event->position().toPoint());
    if (index.data(HeaderRole).toBool()) {
        return;
    }
    m_pressPos = event->position().toPoint();
    m_pressIndex = event->button() == Qt::LeftButton ? QPersistentModelIndex(index) : QPersistentModelIndex();
    QListView::mousePressEvent(event);
}

void FinderSidebar::mouseMoveEvent(QMouseEvent *event)
{
    // Dragging a favorite reorders it, as in Finder.
    if ((event->buttons() & Qt::LeftButton) && m_pressIndex.isValid() && m_pressIndex.data(FavoriteRole).toBool()
        && (event->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        const QModelIndex index = m_pressIndex;
        m_pressIndex = QPersistentModelIndex();
        auto *mime = new QMimeData;
        mime->setData(PlaceMime, QByteArray::number(index.data(PlaceIndexRole).toInt()));
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        const QRect rect = visualRect(index);
        drag->setPixmap(viewport()->grab(rect));
        drag->setHotSpot(event->position().toPoint() - rect.topLeft());
        drag->exec(Qt::MoveAction);
        setUrl(m_url); // the press selected the dragged row
        return;
    }
    QListView::mouseMoveEvent(event);
}

FinderSidebar::DropTarget FinderSidebar::dropTargetAt(const QPoint &pos, const QMimeData *mime) const
{
    const bool internal = mime->hasFormat(PlaceMime);
    if (!internal && !mime->hasUrls()) {
        return {};
    }
    const QList<QUrl> urls = mime->urls();
    // Only folders pin to the sidebar (local ones must exist as folders).
    bool folders = internal || !urls.isEmpty();
    for (const QUrl &url : urls) {
        folders = folders && (!url.isLocalFile() || QFileInfo(url.toLocalFile()).isDir());
    }

    QList<int> favorites; // sidebar rows
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->index(row, 0).data(FavoriteRole).toBool()) {
            favorites.append(row);
        }
    }
    auto gapBefore = [&](qsizetype i) {
        DropTarget target;
        if (!folders || favorites.isEmpty()) {
            return target;
        }
        if (i < favorites.size()) {
            const QModelIndex index = m_model->index(favorites[i], 0);
            target.beforePlace = index.data(PlaceIndexRole).toInt();
            target.lineY = visualRect(index).top();
        } else {
            const QModelIndex index = m_model->index(favorites.last(), 0);
            target.beforePlace = index.data(PlaceIndexRole).toInt() + 1;
            target.lineY = visualRect(index).bottom() + 1;
        }
        return target;
    };

    const QModelIndex index = indexAt(pos);
    if (!index.isValid()) {
        return {};
    }
    const QRect rect = visualRect(index);
    if (index.data(HeaderRole).toBool()) {
        // The Favorites heading pins at the top, the next heading at the end.
        if (!favorites.isEmpty() && index.row() == favorites.first() - 1) {
            return gapBefore(0);
        }
        if (!favorites.isEmpty() && index.row() == favorites.last() + 1) {
            return gapBefore(favorites.size());
        }
        return {};
    }

    // Files dropped on the middle of an item go into it.
    auto intoTarget = [&]() {
        DropTarget target;
        if (internal) {
            return target;
        }
        const QUrl url = index.data(UrlRole).toUrl();
        if (url.scheme() == QLatin1String("recentlyused")) {
            return target;
        }
        if (const FinderCloud *cloud = cloudAt(index); cloud && cloud->state() != FinderCloud::Ready) {
            return target;
        }
        if (url == AirDropUrl()) {
            for (const QUrl &file : urls) {
                if (!file.isLocalFile()) {
                    return target; // AirDrop sends local files only
                }
            }
        } else if (const QModelIndex place = placeIndex(index); place.isValid() && m_places->setupNeeded(place)) {
            return target; // an unmounted device
        }
        target.into = index;
        return target;
    };

    if (index.data(FavoriteRole).toBool()) {
        const qsizetype i = favorites.indexOf(index.row());
        const int quarter = rect.height() / 4;
        if (!internal && pos.y() >= rect.top() + quarter && pos.y() < rect.bottom() - quarter) {
            if (DropTarget into = intoTarget(); into.isValid()) {
                return into;
            }
        }
        return gapBefore(pos.y() < rect.center().y() ? i : i + 1);
    }
    return intoTarget();
}

void FinderSidebar::setDropTarget(const DropTarget &target)
{
    if (target.beforePlace != m_drop.beforePlace || target.lineY != m_drop.lineY || target.into != m_drop.into) {
        m_drop = target;
        viewport()->update();
    }
}

void FinderSidebar::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(PlaceMime) || event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void FinderSidebar::dragMoveEvent(QDragMoveEvent *event)
{
    const DropTarget target = dropTargetAt(event->position().toPoint(), event->mimeData());
    setDropTarget(target);
    if (!target.isValid()) {
        event->ignore();
        return;
    }
    if (target.beforePlace >= 0) {
        // Pinning moves a favorite, or links a folder; it never touches files.
        if (event->mimeData()->hasFormat(PlaceMime)) {
            event->setDropAction(Qt::MoveAction);
        } else {
            event->setDropAction(event->possibleActions() & Qt::LinkAction ? Qt::LinkAction : Qt::CopyAction);
        }
        event->accept();
    } else {
        event->acceptProposedAction();
    }
}

void FinderSidebar::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropTarget({});
    QListView::dragLeaveEvent(event);
}

void FinderSidebar::dropEvent(QDropEvent *event)
{
    const DropTarget target = dropTargetAt(event->position().toPoint(), event->mimeData());
    setDropTarget({});
    if (!target.isValid()) {
        event->ignore();
        return;
    }
    if (target.beforePlace >= 0) {
        if (event->mimeData()->hasFormat(PlaceMime)) {
            m_places->movePlace(event->mimeData()->data(PlaceMime).toInt(), target.beforePlace);
            event->setDropAction(Qt::MoveAction);
        } else {
            pin(event->mimeData()->urls(), target.beforePlace);
            // Tell the source nothing moved, so it doesn't delete anything.
            event->setDropAction(Qt::LinkAction);
        }
        event->accept();
        return;
    }
    const QUrl url = target.into.data(UrlRole).toUrl();
    if (url == AirDropUrl()) {
        FinderAirDrop::instance()->share(event->mimeData()->urls(), window());
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    // Move, copy or link, with Dolphin's menu when no modifier says which.
    DragAndDropHelper::dropUrls(url, event, window());
}

void FinderSidebar::pin(const QList<QUrl> &urls, int beforePlace)
{
    // Rows shift as places are added; keep hold of the place to insert before.
    const QPersistentModelIndex anchor = m_places->index(beforePlace, 0);
    for (const QUrl &url : urls) {
        int row = -1;
        for (int r = 0; r < m_places->rowCount(); ++r) {
            const QModelIndex place = m_places->index(r, 0);
            if (m_places->groupType(place) == KFilePlacesModel::PlacesType && m_places->url(place).matches(url, QUrl::StripTrailingSlash)) {
                row = r; // already in the sidebar: just move it
                break;
            }
        }
        if (row < 0) {
            QString name = url.fileName();
            if (name.isEmpty()) {
                name = url.isLocalFile() ? url.toLocalFile() : url.toDisplayString(QUrl::PreferLocalFile);
            }
            m_places->addPlace(name, url, KIO::iconNameForUrl(url));
            for (int r = m_places->rowCount() - 1; r >= 0; --r) {
                if (m_places->url(m_places->index(r, 0)).matches(url, QUrl::StripTrailingSlash)) {
                    row = r;
                    break;
                }
            }
        }
        if (row >= 0) {
            m_places->movePlace(row, anchor.isValid() ? anchor.row() : m_places->rowCount());
        }
    }
}

void FinderSidebar::paintEvent(QPaintEvent *event)
{
    QListView::paintEvent(event);
    if (!m_drop.isValid()) {
        return;
    }
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    if (m_drop.into.isValid()) {
        // Finder outlines the item a drop goes into.
        const QRectF pill = QRectF(visualRect(m_drop.into)).adjusted(SideMargin + 1, 2, -SideMargin - 1, -2);
        QPainterPath path;
        path.addRoundedRect(pill, 6, 6);
        QColor fill = finderAccent();
        fill.setAlpha(60);
        p.fillPath(path, fill);
        p.setPen(QPen(finderAccent(), 2));
        p.drawPath(path);
    } else {
        // And draws an insertion line, with a ring at its start, between rows.
        const qreal y = qBound(2, m_drop.lineY, viewport()->height() - 2);
        const qreal left = SideMargin + 6;
        p.setPen(QPen(finderAccent(), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(left + 7, y), QPointF(viewport()->width() - SideMargin, y));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(left + 3, y), 3.5, 3.5);
    }
}

void FinderSidebar::mouseReleaseEvent(QMouseEvent *event)
{
    m_pressIndex = QPersistentModelIndex();
    QListView::mouseReleaseEvent(event);
    const QModelIndex index = indexAt(event->position().toPoint());
    if (!index.isValid() || index.data(HeaderRole).toBool()) {
        return;
    }
    if (event->button() == Qt::LeftButton) {
        activate(index, event->modifiers() & Qt::ControlModifier);
    } else if (event->button() == Qt::MiddleButton) {
        activate(index, true);
    }
}

void FinderSidebar::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex index = indexAt(event->pos());
    const QModelIndex place = placeIndex(index);
    FinderCloud *cloud = cloudAt(index);
    QMenu menu(this);
    if (!place.isValid() && !cloud) {
        // Empty space or a heading: offer the drives not connected yet.
        for (FinderCloud *drive : FinderCloud::all()) {
            if (!drive->hasAccount()) {
                menu.addAction(QIcon::fromTheme(drive->iconName()), i18nc("@action:inmenu Finder, %1 is e.g. iCloud Drive", "Connect %1…", drive->name()), this,
                               [this, drive]() {
                                   connectCloud(drive->provider());
                               });
            }
        }
        if (!menu.isEmpty()) {
            menu.exec(event->globalPos());
        }
        return;
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("folder-open")), i18nc("@action:inmenu", "Open"), this, [this, index]() {
        activate(index, false);
    });
    menu.addAction(QIcon::fromTheme(QStringLiteral("tab-new")), i18nc("@action:inmenu", "Open in New Tab"), this, [this, index]() {
        activate(index, true);
    });
    if (cloud) {
        menu.addSeparator();
        cloudMenu(menu, cloud);
        menu.exec(event->globalPos());
        return;
    }
    if (QAction *teardown = m_places->teardownActionForIndex(place)) {
        menu.addSeparator();
        teardown->setParent(&menu);
        menu.addAction(teardown);
        connect(teardown, &QAction::triggered, this, [this, place]() {
            m_places->requestTeardown(place);
        });
    }
    if (QAction *eject = m_places->ejectActionForIndex(place)) {
        eject->setParent(&menu);
        menu.addAction(eject);
        connect(eject, &QAction::triggered, this, [this, place]() {
            m_places->requestEject(place);
        });
    }
    if (!m_places->isDevice(place) && m_places->groupType(place) == KFilePlacesModel::PlacesType) {
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:inmenu", "Remove from Sidebar"), this, [this, place]() {
            m_places->removePlace(place);
        });
    }
    menu.exec(event->globalPos());
}

void FinderSidebar::changeEvent(QEvent *event)
{
    // Icons switch between accent and gray with window activation.
    if (event->type() == QEvent::ActivationChange) {
        viewport()->update();
    }
    QListView::changeEvent(event);
}
