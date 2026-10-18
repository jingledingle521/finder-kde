/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERAIRDROP_H
#define FINDERAIRDROP_H

#include <QIcon>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QProcess;

/**
 * @brief Finder's AirDrop window, backed by the omdrop command-line tool.
 *
 * Shows nearby Apple devices as round tiles (drop files on one, or click it
 * to pick files), and the "Allow me to be discovered by" control. omdrop
 * (github.com/netmojo-systems/omdrop-plugin) with the omdrop-owl radio
 * backend does the actual AirDrop; on Intel Wi-Fi that radio takes the card
 * over, so Wi-Fi drops while AirDrop is on, which the panel says up front.
 */
class FinderAirDropView : public QWidget
{
    Q_OBJECT

public:
    explicit FinderAirDropView(QWidget *parent = nullptr);
    ~FinderAirDropView() override;

    /** Files to send to the next device the user picks (from "AirDrop…"). */
    void setPendingFiles(const QList<QUrl> &files);

    /** The AirDrop glyph: a dot inside widening arcs. */
    static QIcon icon(const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Peer {
        QString mac;
        QString name;
    };

    void runOmdrop(const QStringList &args, std::function<void(int, const QByteArray &, const QByteArray &)> done);
    void checkReady();
    void refreshStatus();
    void refreshPeers();
    void stopPeers();
    void setDiscoverable(bool on);
    void showPeers(const QList<Peer> &peers);
    void send(const Peer &peer, const QList<QUrl> &files);
    void setState(const QString &text);

    QString m_omdrop;
    bool m_ready = false;
    bool m_visible = false; // the radio is up (Wi-Fi is off on Intel)
    bool m_sending = false;
    QList<QUrl> m_pending;

    QTimer m_statusTimer;
    QPointer<QProcess> m_peersProcess;
    QTimer m_peersTimer;

    QWidget *m_peerArea;
    QHBoxLayout *m_peerLayout;
    QLabel *m_lead;
    QLabel *m_state;
    QLabel *m_warning;
    QComboBox *m_discover;
};

#endif
