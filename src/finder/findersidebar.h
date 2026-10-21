/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERSIDEBAR_H
#define FINDERSIDEBAR_H

#include <QListView>
#include <QPersistentModelIndex>
#include <QUrl>

class KFilePlacesModel;
class QMenu;
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

    /** The sidebar's AirDrop item; setUrl(AirDropUrl()) highlights it. */
    static QUrl AirDropUrl();

    /** Whether to list AirDrop (not in the file chooser). */
    void setAirDropShown(bool shown);

    /** Opens the sheet that connects Google Drive, then opens the drive. */
    void connectCloud();

    enum Roles {
        UrlRole = Qt::UserRole + 1,
        HeaderRole,
        PlaceIndexRole, // row in the places model
        CloudRole, // the Google Drive item (FinderCloud)
    };

Q_SIGNALS:
    void placeActivated(const QUrl &url);
    void tabRequested(const QUrl &url);
    /** The AirDrop item was clicked. */
    void airDropRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void rebuild();
    void activate(const QModelIndex &index, bool newTab);
    void activateCloud(bool newTab);
    void cloudMenu(QMenu &menu);
    void updateCloud();
    QModelIndex placeIndex(const QModelIndex &row) const;

    KFilePlacesModel *m_places;
    QStandardItemModel *m_model;
    QUrl m_url;
    QPersistentModelIndex m_pendingSetup; // device being mounted before opening
    bool m_pendingSetupNewTab = false;
    bool m_airDropShown = true;
    int m_pendingCloud = 0; // open the drive once mounted: 1 here, 2 in a new tab
    QTimer *m_spinner; // animates the drive's activity indicator
};

#endif
