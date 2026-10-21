/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERSIDEBAR_H
#define FINDERSIDEBAR_H

#include "findercloud.h"

#include <QListView>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QUrl>

class KFilePlacesModel;
class QMenu;
class QMimeData;
class QTimer;
class QStandardItemModel;

/**
 * @brief The macOS Golden Gate Finder sidebar.
 *
 * Shows the user's places from KFilePlacesModel in two sections, Favorites and
 * Locations, with accent-tinted symbol icons and a rounded gray selection.
 * Replaces Dolphin's Places panel in the dock (which stays alive, hidden,
 * because other parts of Dolphin talk to it).
 */
class FinderSidebar : public QListView
{
    Q_OBJECT

public:
    explicit FinderSidebar(KFilePlacesModel *places, QWidget *parent = nullptr);

    /** Highlights the place that best matches the folder being shown. */
    void setUrl(const QUrl &url);
    QUrl url() const;
    KFilePlacesModel *placesModel() const;
    bool isAirDropShown() const;

    /** The sidebar's AirDrop item; setUrl(AirDropUrl()) highlights it. */
    static QUrl AirDropUrl();
    /** The cloud drive a sidebar row stands for, if any. */
    static FinderCloud *cloudAt(const QModelIndex &index);

    /** Whether to list AirDrop (not in the file chooser). */
    void setAirDropShown(bool shown);

    /** Opens the sheet that connects a cloud drive, then opens the drive. */
    void connectCloud(FinderCloud::Provider provider);

    enum Roles {
        UrlRole = Qt::UserRole + 1,
        HeaderRole,
        PlaceIndexRole, // row in the places model
        CloudRole, // a cloud drive's item: its FinderCloud::Provider
        FavoriteRole, // under Favorites: can be reordered, and pinned between
    };

Q_SIGNALS:
    void placeActivated(const QUrl &url);
    void tabRequested(const QUrl &url);
    /** The AirDrop item was clicked. */
    void airDropRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    /** Where a drag would land: between two favorites, or onto an item. */
    struct DropTarget {
        int beforePlace = -1; // pin before this places-model row (rowCount: at the end)
        int lineY = -1; // the insertion line, in viewport coordinates
        QPersistentModelIndex into; // sidebar row to drop the files into
        bool isValid() const
        {
            return beforePlace >= 0 || into.isValid();
        }
    };
    DropTarget dropTargetAt(const QPoint &pos, const QMimeData *mime) const;
    void setDropTarget(const DropTarget &target);
    void pin(const QList<QUrl> &urls, int beforePlace);

    void rebuild();
    void activate(const QModelIndex &index, bool newTab);
    void activateCloud(FinderCloud *cloud, bool newTab);
    void cloudMenu(QMenu &menu, FinderCloud *cloud);
    void updateCloud();
    QModelIndex placeIndex(const QModelIndex &row) const;

    KFilePlacesModel *m_places;
    QStandardItemModel *m_model;
    QUrl m_url;
    QPersistentModelIndex m_pendingSetup; // device being mounted before opening
    bool m_pendingSetupNewTab = false;
    bool m_airDropShown = true;
    QPointer<FinderCloud> m_pendingCloud; // open this drive once it's mounted
    bool m_pendingCloudNewTab = false;
    QTimer *m_spinner; // animates the drive's activity indicator
    QPoint m_pressPos; // where a drag to reorder favorites may start
    QPersistentModelIndex m_pressIndex;
    DropTarget m_drop; // drawn while dragging over the sidebar
};

#endif
