/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findersidebar.h"
#include "finderairdrop.h"
#include "findercloud.h"
#include "findercloudsheet.h"

#include <KFilePlacesModel>
#include <KLocalizedString>

#include <QAction>
#include <QContextMenuEvent>
#include <QDesktopServices>
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

namespace
{
constexpr int RowHeight = 30;
constexpr int HeaderHeight = 34;
constexpr int SideMargin = 10; // selection inset from the sidebar edges
constexpr int IconSize = 18;

const QColor Accent(0x0a, 0x84, 0xff); // macOS dark-mode system blue
const QColor HeaderColor(255, 255, 255, 110);
const QColor TextColor(255, 255, 255, 225);
const QColor SelectionColor(255, 255, 255, 34);
const QColor HoverColor(255, 255, 255, 14);

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
        static const QIcon airDrop = FinderAirDropView::icon(Qt::white);
        const QIcon icon = iconName == QLatin1String("finder-airdrop") ? airDrop : QIcon::fromTheme(iconName);
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        const QPixmap pm = tinted(icon, IconSize, dpr, active ? Accent : QColor(160, 160, 165));
        const int iconX = int(pill.left()) + 8;
        p->drawPixmap(QPoint(iconX, r.top() + (r.height() - IconSize) / 2), pm);

        // Google Drive: a spinner while syncing or connecting, a badge when
        // it needs the user.
        int statusWidth = 0;
        if (index.data(FinderSidebar::CloudRole).toBool()) {
            const auto *cloud = FinderCloud::instance();
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

    connect(m_places, &QAbstractItemModel::rowsInserted, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::rowsRemoved, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::dataChanged, this, &FinderSidebar::rebuild);
    connect(m_places, &QAbstractItemModel::modelReset, this, &FinderSidebar::rebuild);
    connect(m_places, &KFilePlacesModel::setupDone, this, [this](const QModelIndex &index, bool success) {
        if (success && index == m_pendingSetup) {
            const QUrl url = m_places->url(index);
            m_pendingSetupNewTab ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
        }
        m_pendingSetup = QPersistentModelIndex();
    });

    auto *cloud = FinderCloud::instance();
    connect(cloud, &FinderCloud::stateChanged, this, [this](FinderCloud::State state) {
        rebuild();
        if (state == FinderCloud::Ready && m_pendingCloud) {
            const QUrl url = QUrl::fromLocalFile(FinderCloud::mountPath());
            m_pendingCloud == 2 ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
            m_pendingCloud = 0;
        } else if (state == FinderCloud::SignedOut || state == FinderCloud::Failed || state == FinderCloud::NotConnected) {
            m_pendingCloud = 0;
        }
    });
    connect(cloud, &FinderCloud::activityChanged, this, &FinderSidebar::updateCloud);
    m_spinner->setInterval(33);
    connect(m_spinner, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
    rebuild();
}

void FinderSidebar::rebuild()
{
    QList<QStandardItem *> top; // Recents, above the first heading
    QList<QStandardItem *> favorites;
    QList<QStandardItem *> locations;
    QStandardItem *trash = nullptr;

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
        } else if (url.isLocalFile() && url.toLocalFile() == FinderCloud::mountPath()) {
            continue; // the drive's mount (or a bookmark of it); listed below as Google Drive
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
    if (!locations.isEmpty()) {
        header(i18nc("@title Finder sidebar section", "Locations"));
        // AirDrop leads Locations, as in Finder; it opens the AirDrop panel.
        if (m_airDropShown) {
            auto *airDrop = new QStandardItem(i18nc("@item:inlistbox Finder sidebar", "AirDrop"));
            airDrop->setData(QStringLiteral("finder-airdrop"), Qt::DecorationRole);
            airDrop->setData(AirDropUrl(), UrlRole);
            m_model->appendRow(airDrop);
        }
        // Cloud storage follows AirDrop, as in Finder.
        if (FinderCloud::instance()->hasAccount()) {
            auto *drive = new QStandardItem(i18nc("@item:inlistbox Finder sidebar", "Google Drive"));
            const QString icon = QStringLiteral("folder-google-drive-symbolic");
            drive->setData(QIcon::hasThemeIcon(icon) ? icon : QStringLiteral("folder-cloud-symbolic"), Qt::DecorationRole);
            drive->setData(QUrl::fromLocalFile(FinderCloud::mountPath()), UrlRole);
            drive->setData(true, CloudRole);
            m_model->appendRow(drive);
        }
        for (auto *item : std::as_const(locations)) {
            m_model->appendRow(item);
        }
    }
    setUrl(m_url);
    updateCloud();
}

void FinderSidebar::updateCloud()
{
    const auto *cloud = FinderCloud::instance();
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (QStandardItem *item = m_model->item(row); item->data(CloudRole).toBool()) {
            item->setToolTip(i18nc("@info:tooltip Finder sidebar, %1 is e.g. Up to date", "Google Drive — %1", cloud->activityText()));
        }
    }
    const bool spinning = cloud->hasAccount() && (cloud->isBusy() || cloud->state() == FinderCloud::Starting);
    if (spinning && !m_spinner->isActive()) {
        m_spinner->start();
    } else if (!spinning) {
        m_spinner->stop();
    }
    viewport()->update();
}

void FinderSidebar::connectCloud()
{
    auto *sheet = new FinderCloudSheet(window());
    sheet->setAttribute(Qt::WA_DeleteOnClose);
    connect(sheet, &QDialog::accepted, this, [this]() {
        Q_EMIT placeActivated(QUrl::fromLocalFile(FinderCloud::mountPath()));
    });
    sheet->open();
}

void FinderSidebar::activateCloud(bool newTab)
{
    auto *cloud = FinderCloud::instance();
    switch (cloud->state()) {
    case FinderCloud::Ready: {
        const QUrl url = QUrl::fromLocalFile(FinderCloud::mountPath());
        newTab ? Q_EMIT tabRequested(url) : Q_EMIT placeActivated(url);
        return;
    }
    case FinderCloud::SignedOut:
        connectCloud();
        return;
    case FinderCloud::NotConnected:
    case FinderCloud::SigningIn:
        return;
    case FinderCloud::Starting:
    case FinderCloud::Failed:
        // Open it as soon as it's mounted.
        m_pendingCloud = newTab ? 2 : 1;
        cloud->start();
        return;
    }
}

void FinderSidebar::cloudMenu(QMenu &menu)
{
    auto *cloud = FinderCloud::instance();
    QAction *status = menu.addAction(cloud->activityText());
    status->setEnabled(false);
    menu.addSeparator();
    if (cloud->state() == FinderCloud::SignedOut) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("im-user-online")), i18nc("@action:inmenu Finder Google Drive", "Sign In Again…"), this, &FinderSidebar::connectCloud);
    } else if (cloud->state() == FinderCloud::Failed) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18nc("@action:inmenu Finder Google Drive", "Try Again"), cloud, &FinderCloud::start);
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), i18nc("@action:inmenu Finder Google Drive", "Open Google Drive on the Web"), this, []() {
        QDesktopServices::openUrl(FinderCloud::webUrl());
    });
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("network-disconnect")), i18nc("@action:inmenu Finder Google Drive", "Disconnect Google Drive…"), this, [this]() {
        auto *cloud = FinderCloud::instance();
        QString text = i18nc("@info Finder Google Drive",
                             "Your files stay in Google Drive. Finder removes the drive from this computer, along with its local copies, "
                             "and signs this computer out.");
        if (cloud->uploads() > 0 || cloud->uploadErrors() > 0) {
            text = i18ncp("@info Finder Google Drive",
                          "1 file hasn't finished uploading. If you disconnect now, the changes to it are lost.",
                          "%1 files haven't finished uploading. If you disconnect now, the changes to them are lost.",
                          cloud->uploads() + cloud->uploadErrors());
        }
        QMessageBox box(QMessageBox::Warning, i18nc("@title:window", "Disconnect Google Drive?"), i18nc("@title Finder Google Drive", "Disconnect Google Drive?"),
                        QMessageBox::Cancel, window());
        box.setInformativeText(text);
        QPushButton *disconnect = box.addButton(i18nc("@action:button Finder Google Drive", "Disconnect"), QMessageBox::DestructiveRole);
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
    if (index.data(CloudRole).toBool()) {
        activateCloud(newTab);
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
    QListView::mousePressEvent(event);
}

void FinderSidebar::mouseReleaseEvent(QMouseEvent *event)
{
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
    const bool cloud = index.data(CloudRole).toBool();
    QMenu menu(this);
    if (!place.isValid() && !cloud) {
        // Empty space or a heading: offer the drive if there isn't one.
        if (!FinderCloud::instance()->hasAccount()) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("folder-cloud")), i18nc("@action:inmenu Finder", "Connect Google Drive…"), this, &FinderSidebar::connectCloud);
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
        cloudMenu(menu);
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
