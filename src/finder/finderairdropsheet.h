/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERAIRDROPSHEET_H
#define FINDERAIRDROPSHEET_H

#include <QDialog>
#include <QUrl>

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QStackedWidget;

/**
 * @brief Finder's Share › AirDrop sheet.
 *
 * Opens over the current folder: first a warning that Wi-Fi drops while
 * AirDrop is on (skipped if AirDrop is already on, or if the user asked not
 * to be warned again), then the nearby devices. Picking one starts the send
 * and closes the sheet; its progress shows in the window's transfer card.
 * If the sheet turned AirDrop on, it turns it off again afterwards.
 */
class FinderAirDropSheet : public QDialog
{
    Q_OBJECT

public:
    FinderAirDropSheet(const QList<QUrl> &files, QWidget *parent);
    ~FinderAirDropSheet() override;

private:
    void turnOn();
    void showDevices();
    void updatePeers();

    QList<QUrl> m_files;
    bool m_turnedOn = false; // this sheet switched AirDrop on
    bool m_sent = false;

    QStackedWidget *m_pages;
    QCheckBox *m_dontWarn;
    QPushButton *m_turnOnButton;
    QLabel *m_warningState;
    QWidget *m_peerArea;
    QHBoxLayout *m_peerLayout;
    QLabel *m_searching;
};

#endif
