/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERFILEDIALOG_H
#define FINDERFILEDIALOG_H

#include "filechooserportal.h"

#include <QMainWindow>
#include <QUrl>

class DolphinView;
class FinderSidebar;
class KFilePlacesModel;
class KUrlNavigator;
class QAction;
class QComboBox;
class QLineEdit;
class QPushButton;

/**
 * @brief The Finder-style Open/Save window behind the file chooser portal.
 *
 * Built from Finder's own parts: its window chrome (rounded, stoplights,
 * glass sidebar, toolbar capsules and folder title), its sidebar, and
 * Dolphin's file view with Finder's list and icon styling, so it looks like a
 * Finder window with Open/Save controls along the bottom.
 */
class FinderFileDialog : public QMainWindow
{
    Q_OBJECT

public:
    enum Mode {
        Open,
        Save,
    };

    struct Options {
        Mode mode = Open;
        QString title;
        QString acceptLabel;
        bool multiple = false;
        bool directory = false; // choose a folder rather than files
        PortalFilterList filters;
        int currentFilter = 0;
        QString currentName; // Save: the suggested file name
        QUrl folder; // where to start
    };

    explicit FinderFileDialog(const Options &options);

    const Options &options() const;

    /** Shows the window and gives it focus. */
    void present();

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    /** The dialog is done: the chosen URLs (if @p accepted) and the filter in use. */
    void finished(bool accepted, const QList<QUrl> &urls, int filter);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setFolder(const QUrl &url);
    void applyFilter(int index);
    void updateAcceptButton();
    void accept();
    void finish(bool accepted, const QList<QUrl> &urls);
    QList<QUrl> chosenFiles() const;

    Options m_options;
    bool m_done = false;

    KFilePlacesModel *m_places;
    KUrlNavigator *m_navigator;
    DolphinView *m_view;
    FinderSidebar *m_sidebar;
    QAction *m_back;
    QAction *m_forward;
    QAction *m_icons;
    QAction *m_details;
    QLineEdit *m_name = nullptr; // Save only
    QComboBox *m_filter = nullptr;
    QPushButton *m_accept;
};

#endif
