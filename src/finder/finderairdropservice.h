/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERAIRDROPSERVICE_H
#define FINDERAIRDROPSERVICE_H

#include <QHash>
#include <QList>
#include <QObject>
#include <memory>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <functional>

class QProcess;

/** A nearby Apple device, as `omdrop peers -n --json` lists it. */
struct FinderAirDropPeer {
    QString mac;
    QString name;
};

/** One AirDrop transfer, in either direction, for the progress cards. */
struct FinderTransfer {
    enum State {
        Waiting, // asked the other side, not accepted yet
        Running,
        Done,
        Failed,
    };
    int id = 0;
    bool incoming = false;
    QString peer; // the other device's name
    QString title; // a file name, or "3 items"
    QStringList files; // local paths (sent files, or stored received files)
    qint64 total = 0; // bytes, 0 if unknown
    qint64 done = 0;
    QString detail; // the current step, in words
    State state = Waiting;
};

/**
 * @brief Finder's link to omdrop, the AirDrop tool: discovery, peers,
 * sending and watching incoming transfers.
 *
 * One per process. Sends run `omdrop send`; their progress is read from how
 * far the sending process has read each file (/proc/PID/fdinfo). Incoming
 * transfers are followed in the receiver's log (airdrop-receiver.service),
 * and their progress from the size of the file it is writing.
 */
class FinderAirDrop : public QObject
{
    Q_OBJECT

public:
    static FinderAirDrop *instance();

    QString omdrop() const;
    /** Whether omdrop can receive and send (`omdrop doctor`). */
    void checkReady(std::function<void(bool ready, const QString &why)> done);

    /** The radio is up: visible to Apple devices (and Wi-Fi is off on Intel cards). */
    bool isVisible() const;
    void refreshStatus();
    void setDiscoverable(bool on, std::function<void(bool ok, const QString &error)> done = {});

    /** Keeps the peer list fresh while @p watcher is registered and AirDrop is visible. */
    void watchPeers(QObject *watcher);
    void unwatchPeers(QObject *watcher);
    QList<FinderAirDropPeer> peers() const;

    /** Sends @p files; with @p turnOffAfter, AirDrop is switched off once it ends. */
    void send(const FinderAirDropPeer &peer, const QList<QUrl> &files, bool turnOffAfter);
    /** Cancels a running send. */
    void cancel(int transferId);

    /**
     * Finder's Share › AirDrop: warns about Wi-Fi, turns AirDrop on and lets
     * the user pick a device, without leaving the current folder.
     */
    void share(const QList<QUrl> &files, QWidget *parent);

Q_SIGNALS:
    void visibleChanged(bool visible);
    void peersChanged();
    void transferChanged(const FinderTransfer &transfer);

private:
    // Zips the next folder of a share (see share()), then opens the sheet.
    void zipNextFolder(std::shared_ptr<void> state);
    FinderAirDrop();
    void run(const QStringList &args, std::function<void(int, const QByteArray &, const QByteArray &)> done);
    void refreshPeers();
    void stopPeers();
    void followReceiver();
    void parseReceiverLine(const QString &line);
    void pollProgress();
    void finish(FinderTransfer &transfer, FinderTransfer::State state, const QString &detail);
    void notify(const FinderTransfer &transfer);

    QString m_omdrop;
    bool m_visible = false;
    QTimer m_statusTimer;

    QSet<QObject *> m_peerWatchers;
    QPointer<QProcess> m_peersProcess;
    QTimer m_peersTimer;
    QList<FinderAirDropPeer> m_peers;

    int m_nextId = 1;
    QHash<int, FinderTransfer> m_transfers;
    QHash<int, QPointer<QProcess>> m_sends; // running `omdrop send`s
    QSet<int> m_turnOffAfter;
    QTimer m_progressTimer;

    QPointer<QProcess> m_journal; // following the receiver's log
    int m_incoming = 0; // the incoming transfer being received, if any
    QHash<QString, int> m_uploads; // receiver TransferID -> transfer id
    qint64 m_lastIncomingBytes = -1;
    int m_incomingStalls = 0;
    qint64 m_receiverPid = 0; // airdrop-receiver's main process, looked up in the background
    QPointer<QProcess> m_pidQuery;
};

/** A nearby device as a round tile with initials; files can be dropped on it. */
class FinderPeerTile : public QWidget
{
public:
    FinderPeerTile(const FinderAirDropPeer &peer, std::function<void(const QList<QUrl> &)> onFiles, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    FinderAirDropPeer m_peer;
    std::function<void(const QList<QUrl> &)> m_onFiles;
    bool m_hot = false;
};

#endif
