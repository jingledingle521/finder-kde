/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERAIRDROP_H
#define FINDERAIRDROP_H

#include <QIcon>
#include <QUrl>
#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;

/**
 * @brief Finder's AirDrop window (Go › AirDrop), backed by FinderAirDrop.
 *
 * Shows nearby Apple devices as round tiles (drop files on one, or click it
 * to pick files) and the "Allow me to be discovered by" control. On Intel
 * Wi-Fi the AirDrop radio takes the card over, so Wi-Fi drops while AirDrop
 * is on, which the panel says up front.
 */
class FinderAirDropView : public QWidget
{
    Q_OBJECT

public:
    explicit FinderAirDropView(QWidget *parent = nullptr);

    /** Files to send to the next device the user picks. */
    void setPendingFiles(const QList<QUrl> &files);

    /** The AirDrop glyph: a dot inside widening arcs. */
    static QIcon icon(const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void updateState();
    void updatePeers();
    void setState(const QString &text);

    bool m_ready = false;
    QString m_notReady;
    QList<QUrl> m_pending;

    QWidget *m_peerArea;
    QHBoxLayout *m_peerLayout;
    QLabel *m_lead;
    QLabel *m_state;
    QLabel *m_warning;
    QComboBox *m_discover;
};

#endif
