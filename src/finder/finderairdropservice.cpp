/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "finderairdropservice.h"
#include <memory>
#include <functional>
#include <QPointer>
#include <QMessageBox>
#include <QGuiApplication>
#include <QDateTime>
#include "finderairdropsheet.h"

#include <KLocalizedString>
#include <KNotification>

#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <sys/stat.h>

namespace
{
const QColor Accent(0x0a, 0x84, 0xff);

/** Child processes of @p root, recursively, read from /proc. */
QList<qint64> descendants(qint64 root)
{
    QHash<qint64, QList<qint64>> children;
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList({}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool ok = false;
        const qint64 n = pid.toLongLong(&ok);
        if (!ok) {
            continue;
        }
        QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!stat.open(QIODevice::ReadOnly)) {
            continue;
        }
        // "pid (comm) state ppid ...": comm may hold spaces, so split after ')'.
        const QByteArray line = stat.readAll();
        const int close = line.lastIndexOf(')');
        const QList<QByteArray> fields = line.mid(close + 2).split(' ');
        if (fields.size() > 1) {
            children[fields.at(1).toLongLong()].append(n);
        }
    }
    QList<qint64> out{root};
    for (int i = 0; i < out.size(); ++i) {
        out += children.value(out.at(i));
    }
    return out;
}

/** For each open file of @p pid: its path (readlink of /proc/PID/fd/N) and fd number. */
QList<std::pair<QString, QString>> openFiles(qint64 pid)
{
    QList<std::pair<QString, QString>> out;
    const QString dir = QStringLiteral("/proc/%1/fd").arg(pid);
    const QStringList fds = QDir(dir).entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &fd : fds) {
        const QString target = QFileInfo(dir + QLatin1Char('/') + fd).symLinkTarget();
        if (!target.isEmpty()) {
            out.append({target, fd});
        }
    }
    return out;
}

qint64 fdPosition(qint64 pid, const QString &fd)
{
    QFile info(QStringLiteral("/proc/%1/fdinfo/%2").arg(pid).arg(fd));
    if (!info.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QByteArray first = info.readLine(); // "pos:\t12345"
    return first.startsWith("pos:") ? first.mid(4).trimmed().toLongLong() : -1;
}

qint64 fdSize(qint64 pid, const QString &fd)
{
    struct stat st;
    const QByteArray path = QStringLiteral("/proc/%1/fd/%2").arg(pid).arg(fd).toLocal8Bit();
    return ::stat(path.constData(), &st) == 0 ? qint64(st.st_size) : -1;
}

QString lastLine(const QByteArray &text)
{
    return QString::fromUtf8(text).trimmed().section(QLatin1Char('\n'), -1).trimmed();
}
}

FinderAirDrop *FinderAirDrop::instance()
{
    static FinderAirDrop *self = new FinderAirDrop;
    return self;
}

FinderAirDrop::FinderAirDrop()
{
    // Prefer the user's copy (omdrop setup puts it in ~/.local/bin).
    m_omdrop = QStandardPaths::findExecutable(QStringLiteral("omdrop"), {QDir::homePath() + QStringLiteral("/.local/bin")});
    if (m_omdrop.isEmpty()) {
        m_omdrop = QStandardPaths::findExecutable(QStringLiteral("omdrop"));
    }

    m_statusTimer.setInterval(2500);
    connect(&m_statusTimer, &QTimer::timeout, this, &FinderAirDrop::refreshStatus);
    m_peersTimer.setSingleShot(true);
    m_peersTimer.setInterval(3000);
    connect(&m_peersTimer, &QTimer::timeout, this, &FinderAirDrop::refreshPeers);
    m_progressTimer.setInterval(500);
    connect(&m_progressTimer, &QTimer::timeout, this, &FinderAirDrop::pollProgress);

    if (!m_omdrop.isEmpty()) {
        followReceiver();
    }
}

QString FinderAirDrop::omdrop() const
{
    return m_omdrop;
}

bool FinderAirDrop::isVisible() const
{
    return m_visible;
}

QList<FinderAirDropPeer> FinderAirDrop::peers() const
{
    return m_peers;
}

void FinderAirDrop::run(const QStringList &args, std::function<void(int, const QByteArray &, const QByteArray &)> done)
{
    if (m_omdrop.isEmpty()) {
        done(-1, {}, {});
        return;
    }
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus) {
        done(code, process->readAllStandardOutput(), process->readAllStandardError());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            done(-1, {}, {});
            process->deleteLater();
        }
    });
    process->start(m_omdrop, args);
}

void FinderAirDrop::checkReady(std::function<void(bool, const QString &)> done)
{
    if (m_omdrop.isEmpty()) {
        done(false, i18nc("@info Finder AirDrop", "AirDrop needs omdrop. Install omdrop-owl and run “omdrop setup”."));
        return;
    }
    run({QStringLiteral("doctor"), QStringLiteral("--json")}, [done](int, const QByteArray &out, const QByteArray &) {
        const QJsonObject doctor = QJsonDocument::fromJson(out).object();
        QStringList missing;
        for (const QJsonValue &item : doctor.value(QStringLiteral("missing")).toArray()) {
            missing.append(item.toObject().value(QStringLiteral("say")).toString());
        }
        const bool ready = doctor.value(QStringLiteral("ready")).toBool();
        done(ready, ready ? QString() : i18nc("@info Finder AirDrop", "AirDrop isn't ready: %1", missing.isEmpty() ? i18n("omdrop did not answer.") : missing.join(QLatin1Char(' '))));
    });
}

void FinderAirDrop::refreshStatus()
{
    run({QStringLiteral("status"), QStringLiteral("--json")}, [this](int, const QByteArray &out, const QByteArray &) {
        const QJsonObject status = QJsonDocument::fromJson(out).object();
        const bool visible = status.value(QStringLiteral("visible")).toBool() || status.value(QStringLiteral("receiving")).toBool();
        if (visible == m_visible) {
            return;
        }
        m_visible = visible;
        if (visible) {
            refreshPeers();
        } else {
            stopPeers();
            m_peers.clear();
            Q_EMIT peersChanged();
        }
        Q_EMIT visibleChanged(visible);
    });
}

void FinderAirDrop::setDiscoverable(bool on, std::function<void(bool, const QString &)> done)
{
    const QStringList args = on ? QStringList{QStringLiteral("on"), QStringLiteral("-e"), QStringLiteral("10m")} : QStringList{QStringLiteral("off")};
    run(args, [this, done](int code, const QByteArray &, const QByteArray &err) {
        refreshStatus();
        if (done) {
            const QString message = lastLine(err);
            done(code == 0, code == 0 ? QString() : (message.isEmpty() ? i18nc("@info Finder AirDrop", "omdrop could not change AirDrop.") : message));
        }
    });
}

void FinderAirDrop::watchPeers(QObject *watcher)
{
    if (m_peerWatchers.contains(watcher)) {
        return;
    }
    m_peerWatchers.insert(watcher);
    connect(watcher, &QObject::destroyed, this, [this, watcher]() {
        unwatchPeers(watcher);
    });
    m_statusTimer.start();
    refreshStatus();
    if (m_visible) {
        refreshPeers();
    }
}

void FinderAirDrop::unwatchPeers(QObject *watcher)
{
    m_peerWatchers.remove(watcher);
    if (m_peerWatchers.isEmpty()) {
        m_statusTimer.stop();
        m_peersTimer.stop();
        stopPeers();
    }
}

void FinderAirDrop::refreshPeers()
{
    if (!m_visible || m_peersProcess || m_peerWatchers.isEmpty() || m_omdrop.isEmpty()) {
        return;
    }
    // A named lookup takes 15-20 seconds; one at a time, then again shortly after.
    auto *process = new QProcess(this);
    m_peersProcess = process;
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
        const QJsonArray array = QJsonDocument::fromJson(process->readAllStandardOutput()).array();
        process->deleteLater();
        if (code == 0 || !array.isEmpty()) {
            QList<FinderAirDropPeer> peers;
            for (const QJsonValue &value : array) {
                const QJsonObject peer = value.toObject();
                const QString name = peer.value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) { // an unnamed device can't be picked by a person
                    peers.append({peer.value(QStringLiteral("mac")).toString(), name});
                }
            }
            m_peers = peers;
            Q_EMIT peersChanged();
        }
        if (m_visible && !m_peerWatchers.isEmpty()) {
            m_peersTimer.start();
        }
    });
    process->start(m_omdrop, {QStringLiteral("peers"), QStringLiteral("-n"), QStringLiteral("--json")});
}

void FinderAirDrop::stopPeers()
{
    if (m_peersProcess) {
        // omdrop runs the lookup as its own process group and cleans up on TERM.
        m_peersProcess->disconnect(this);
        m_peersProcess->terminate();
        m_peersProcess->waitForFinished(2000);
        m_peersProcess->deleteLater();
    }
}

void FinderAirDrop::send(const FinderAirDropPeer &peer, const QList<QUrl> &files, bool turnOffAfter)
{
    FinderTransfer transfer;
    transfer.id = m_nextId++;
    transfer.incoming = false;
    transfer.peer = peer.name;
    for (const QUrl &url : files) {
        transfer.files.append(url.toLocalFile());
        transfer.total += QFileInfo(url.toLocalFile()).size();
    }
    transfer.title = files.size() == 1 ? files.first().fileName() : i18ncp("@info Finder AirDrop", "1 item", "%1 items", files.size());
    transfer.detail = i18nc("@info Finder AirDrop", "Connecting to %1…", peer.name);
    m_transfers.insert(transfer.id, transfer);
    if (turnOffAfter) {
        m_turnOffAfter.insert(transfer.id);
    }
    Q_EMIT transferChanged(transfer);

    auto *process = new QProcess(this);
    m_sends.insert(transfer.id, process);
    const int id = transfer.id;
    // omdrop narrates a send in a few lines; each one moves the card along.
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, id]() {
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine()).trimmed();
            FinderTransfer &t = m_transfers[id];
            if (line.startsWith(QLatin1String("Asking them to accept"))) {
                t.state = FinderTransfer::Waiting;
                t.detail = i18nc("@info Finder AirDrop", "Waiting for %1 to accept…", t.peer);
            } else if (line.startsWith(QLatin1String("They accepted"))) {
                t.state = FinderTransfer::Running;
                t.detail = i18nc("@info Finder AirDrop", "Sending to %1…", t.peer);
                m_progressTimer.start();
            } else {
                continue;
            }
            Q_EMIT transferChanged(t);
        }
    });
    connect(process, &QProcess::finished, this, [this, process, id](int code, QProcess::ExitStatus status) {
        FinderTransfer &t = m_transfers[id];
        const QString error = lastLine(process->readAllStandardError());
        process->deleteLater();
        m_sends.remove(id);
        if (t.state == FinderTransfer::Failed) {
            return; // cancelled
        }
        if (code == 0 && status == QProcess::NormalExit) {
            t.done = t.total;
            finish(t, FinderTransfer::Done, i18nc("@info Finder AirDrop", "Sent to %1", t.peer));
        } else {
            finish(t, FinderTransfer::Failed, error.isEmpty() ? i18nc("@info Finder AirDrop", "The transfer failed.") : error);
        }
    });
    QStringList args{QStringLiteral("send"), QStringLiteral("--to"), peer.mac};
    args += m_transfers[id].files;
    process->start(m_omdrop, args);
}

void FinderAirDrop::cancel(int transferId)
{
    QPointer<QProcess> process = m_sends.value(transferId);
    if (!process) {
        return;
    }
    FinderTransfer &t = m_transfers[transferId];
    t.state = FinderTransfer::Failed; // before finished() fires
    process->terminate();
    finish(t, FinderTransfer::Failed, i18nc("@info Finder AirDrop", "Cancelled"));
}

void FinderAirDrop::finish(FinderTransfer &transfer, FinderTransfer::State state, const QString &detail)
{
    transfer.state = state;
    transfer.detail = detail;
    Q_EMIT transferChanged(transfer);
    notify(transfer);
    if (m_turnOffAfter.remove(transfer.id)) {
        // AirDrop was switched on just for this send: give Wi-Fi back.
        setDiscoverable(false);
    }
}

void FinderAirDrop::notify(const FinderTransfer &transfer)
{
    if (transfer.incoming && transfer.state == FinderTransfer::Done) {
        return; // omdrop already announces each received file
    }
    QString title;
    if (transfer.state == FinderTransfer::Done) {
        title = transfer.incoming ? i18nc("@title Finder AirDrop", "Received “%1”", transfer.title) : i18nc("@title Finder AirDrop", "Sent “%1”", transfer.title);
    } else {
        title = transfer.incoming ? i18nc("@title Finder AirDrop", "Couldn't receive “%1”", transfer.title) : i18nc("@title Finder AirDrop", "Couldn't send “%1”", transfer.title);
    }
    const QString text = transfer.incoming ? i18nc("@info Finder AirDrop", "From %1 — %2", transfer.peer, transfer.detail)
                                           : i18nc("@info Finder AirDrop", "AirDrop to %1 — %2", transfer.peer, transfer.detail);
    KNotification::event(KNotification::Notification, title, text, QStringLiteral("folder-cloud"));
}

namespace
{
// Folders being zipped for AirDrop (FinderAirDrop::share).
struct AirDropZipJob {
    QList<QUrl> ready; // the files to send so far
    QStringList folders; // still to zip
    QString outDir;
    std::function<void(const QList<QUrl> &)> openSheet;
    QPointer<QWidget> owner;
};
}

void FinderAirDrop::share(const QList<QUrl> &files, QWidget *parent)
{
    // AirDrop (omdrop) sends files, not folders: a folder goes as a .zip of
    // it, which iPhones and Macs open with a tap, made in the runtime
    // directory (cleared at logout) before the sheet opens.
    QList<QUrl> ready;
    QStringList folders;
    for (const QUrl &url : files) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()) {
            folders.append(url.toLocalFile());
        } else {
            ready.append(url);
        }
    }
    QPointer<QWidget> owner(parent);
    auto openSheet = [owner](const QList<QUrl> &urls) {
        auto *sheet = new FinderAirDropSheet(urls, owner);
        sheet->setAttribute(Qt::WA_DeleteOnClose);
        sheet->open();
    };
    if (folders.isEmpty()) {
        openSheet(ready);
        return;
    }

    // The zips are made one after another; each step holds the shared
    // state, so it lives exactly as long as the work.
    auto job = std::make_shared<AirDropZipJob>();
    job->ready = ready;
    job->folders = folders;
    job->outDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/finder-airdrop/")
        + QString::number(QDateTime::currentMSecsSinceEpoch());
    job->openSheet = openSheet;
    job->owner = owner;
    QDir().mkpath(job->outDir);
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    zipNextFolder(job);
}

void FinderAirDrop::zipNextFolder(std::shared_ptr<void> state)
{
    auto job = std::static_pointer_cast<AirDropZipJob>(state);
    if (job->folders.isEmpty()) {
        QGuiApplication::restoreOverrideCursor();
        job->openSheet(job->ready);
        return;
    }
    const QFileInfo folder(job->folders.takeFirst());
    const QString zipPath = job->outDir + QLatin1Char('/') + folder.fileName() + QStringLiteral(".zip");
    auto fail = [job](const QString &message) {
        QGuiApplication::restoreOverrideCursor();
        QMessageBox::warning(job->owner, i18nc("@title:window Finder", "AirDrop"), message);
    };
    auto *zip = new QProcess(this);
    zip->setWorkingDirectory(folder.absolutePath());
    connect(zip, &QProcess::finished, this, [this, zip, job, zipPath, folder, fail](int code, QProcess::ExitStatus status) {
        zip->deleteLater();
        if (status != QProcess::NormalExit || code != 0 || !QFileInfo::exists(zipPath)) {
            fail(i18nc("@info Finder", "Couldn't prepare “%1” for AirDrop.", folder.fileName()));
            return;
        }
        job->ready.append(QUrl::fromLocalFile(zipPath));
        zipNextFolder(job);
    });
    connect(zip, &QProcess::errorOccurred, this, [zip, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            zip->deleteLater();
            fail(i18nc("@info Finder", "Sending folders needs the zip program (sudo dnf install zip)."));
        }
    });
    zip->start(QStringLiteral("zip"), {QStringLiteral("-r"), QStringLiteral("-q"), zipPath, folder.fileName()});
}

void FinderAirDrop::followReceiver()
{
    // The receiver logs each Ask and Upload; follow its journal from now on.
    auto *journal = new QProcess(this);
    m_journal = journal;
    connect(journal, &QProcess::readyReadStandardOutput, this, [this, journal]() {
        while (journal->canReadLine()) {
            parseReceiverLine(QString::fromUtf8(journal->readLine()).trimmed());
        }
    });
    connect(journal, &QProcess::finished, this, [this, journal]() {
        journal->deleteLater();
        QTimer::singleShot(5000, this, &FinderAirDrop::followReceiver);
    });
    journal->start(QStringLiteral("journalctl"),
                   {QStringLiteral("--user"), QStringLiteral("-u"), QStringLiteral("airdrop-receiver"), QStringLiteral("-f"), QStringLiteral("-n"), QStringLiteral("0"),
                    QStringLiteral("-o"), QStringLiteral("cat")});
}

void FinderAirDrop::parseReceiverLine(const QString &line)
{
    static const QRegularExpression ask(QStringLiteral(R"(ask from (.+?) \((.*?)\): type \S+, (\d+) item\(s\): (.*)$)"));
    static const QRegularExpression file(QStringLiteral(R"(^(.*) (\S+) (\d+)B$)"));
    static const QRegularExpression uploadStart(QStringLiteral(R"(upload (\S+): \d+% of \d+ free bytes)"));
    static const QRegularExpression uploadRead(QStringLiteral(R"(upload (\S+): (\d+) bytes dvzip)"));
    static const QRegularExpression uploadStored(QStringLiteral(R"(upload (\S+): stored (.*)$)"));
    static const QRegularExpression rejected(QStringLiteral(R"((/\w+|upload \S+) rejected: (.*)$|WARNING .*rejected: (.*)$)"));

    if (const auto m = ask.match(line); m.hasMatch()) {
        FinderTransfer t;
        t.id = m_nextId++;
        t.incoming = true;
        t.peer = m.captured(1);
        QStringList names;
        for (const QString &part : m.captured(4).split(QStringLiteral("; "))) {
            if (const auto f = file.match(part); f.hasMatch()) {
                names.append(f.captured(1));
                t.total += f.captured(3).toLongLong();
            }
        }
        const int count = m.captured(3).toInt();
        t.title = count == 1 && !names.isEmpty() ? names.first() : i18ncp("@info Finder AirDrop", "1 item", "%1 items", count);
        t.detail = i18nc("@info Finder AirDrop", "From %1", t.peer);
        t.state = FinderTransfer::Waiting;
        m_transfers.insert(t.id, t);
        m_incoming = t.id;
        m_lastIncomingBytes = -1;
        m_incomingStalls = 0;
        Q_EMIT transferChanged(t);
        return;
    }
    if (!m_incoming) {
        return;
    }
    FinderTransfer &t = m_transfers[m_incoming];
    if (const auto m = uploadStart.match(line); m.hasMatch()) {
        m_uploads.insert(m.captured(1), t.id);
        t.state = FinderTransfer::Running;
        t.detail = i18nc("@info Finder AirDrop", "Receiving from %1…", t.peer);
        m_progressTimer.start();
        Q_EMIT transferChanged(t);
    } else if (const auto m = uploadRead.match(line); m.hasMatch()) {
        t.done = t.total;
        t.detail = i18nc("@info Finder AirDrop", "Saving…");
        Q_EMIT transferChanged(t);
    } else if (const auto m = uploadStored.match(line); m.hasMatch()) {
        const QString dir = QDir::homePath() + QStringLiteral("/Downloads/");
        t.files.clear();
        for (const QString &name : m.captured(2).split(QStringLiteral(", "))) {
            t.files.append(dir + name);
        }
        t.done = t.total;
        m_incoming = 0;
        finish(t, FinderTransfer::Done, i18nc("@info Finder AirDrop", "From %1 · saved to Downloads", t.peer));
    } else if (const auto m = rejected.match(line); m.hasMatch() && t.state != FinderTransfer::Done) {
        const QString reason = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
        m_incoming = 0;
        finish(t, FinderTransfer::Failed, reason.isEmpty() ? i18nc("@info Finder AirDrop", "The transfer stopped.") : reason);
    }
}

void FinderAirDrop::pollProgress()
{
    bool active = false;

    // Sends: how far the sending processes have read each file.
    for (auto it = m_sends.constBegin(); it != m_sends.constEnd(); ++it) {
        FinderTransfer &t = m_transfers[it.key()];
        if (!it.value() || t.state != FinderTransfer::Running) {
            continue;
        }
        active = true;
        qint64 done = 0;
        int furthest = -1;
        QHash<int, qint64> positions;
        for (const qint64 pid : descendants(it.value()->processId())) {
            for (const auto &[path, fd] : openFiles(pid)) {
                const int index = t.files.indexOf(path);
                if (index >= 0) {
                    positions[index] = std::max(positions.value(index), fdPosition(pid, fd));
                    furthest = std::max(furthest, index);
                }
            }
        }
        if (furthest < 0) {
            continue; // between files, or not reading from disk: keep the last figure
        }
        for (int i = 0; i < t.files.size(); ++i) {
            // Files before the one being read are done; that one, as far as it's read.
            done += i < furthest ? QFileInfo(t.files.at(i)).size() : (i == furthest ? positions.value(i) : 0);
        }
        if (done > t.done) {
            t.done = done;
            Q_EMIT transferChanged(t);
        }
    }

    // Receiving: the size of the file the receiver is writing in Downloads.
    if (m_incoming && m_transfers[m_incoming].state == FinderTransfer::Running) {
        active = true;
        FinderTransfer &t = m_transfers[m_incoming];
        QProcess pidQuery;
        pidQuery.start(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("-p"), QStringLiteral("MainPID"), QStringLiteral("--value"), QStringLiteral("airdrop-receiver")});
        pidQuery.waitForFinished(1000);
        const qint64 pid = pidQuery.readAllStandardOutput().trimmed().toLongLong();
        qint64 bytes = -1;
        const QString downloads = QDir::homePath() + QStringLiteral("/Downloads/");
        if (pid > 0) {
            for (const auto &[path, fd] : openFiles(pid)) {
                if (path.startsWith(downloads)) { // the unnamed temporary file being written
                    bytes = std::max(bytes, fdSize(pid, fd));
                }
            }
        }
        if (bytes >= 0) {
            // The wire format is compressed, so stop short of 100% until it's stored.
            t.done = t.total > 0 ? std::min(bytes, t.total * 99 / 100) : bytes;
            Q_EMIT transferChanged(t);
        }
        // A transfer the phone gave up on leaves no line in the log: notice it going quiet.
        m_incomingStalls = bytes == m_lastIncomingBytes ? m_incomingStalls + 1 : 0;
        m_lastIncomingBytes = bytes;
        if (m_incomingStalls > 120) { // 60 seconds
            m_incoming = 0;
            finish(t, FinderTransfer::Failed, i18nc("@info Finder AirDrop", "The transfer stopped."));
        }
    }

    if (!active) {
        m_progressTimer.stop();
    }
}

FinderPeerTile::FinderPeerTile(const FinderAirDropPeer &peer, std::function<void(const QList<QUrl> &)> onFiles, QWidget *parent)
    : QWidget(parent)
    , m_peer(peer)
    , m_onFiles(std::move(onFiles))
{
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(112, 118);
    setToolTip(i18nc("@info:tooltip Finder AirDrop", "Drop files here, or click to send"));
}

void FinderPeerTile::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF circle((width() - 72) / 2.0, 4, 72, 72);
    QLinearGradient fill(circle.topLeft(), circle.bottomLeft());
    fill.setColorAt(0, m_hot ? Accent.lighter(130) : QColor(150, 150, 160));
    fill.setColorAt(1, m_hot ? Accent : QColor(100, 100, 110));
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(circle);

    // Initials, like a contact without a photo.
    QString initials;
    for (const QString &word : m_peer.name.split(QRegularExpression(QStringLiteral("[\\s’']")), Qt::SkipEmptyParts)) {
        if (initials.size() < 2 && word.at(0).isLetterOrNumber() && word != QLatin1String("s")) {
            initials += word.at(0).toUpper();
        }
    }
    QFont big = font();
    big.setPointSizeF(big.pointSizeF() * 1.8);
    big.setWeight(QFont::DemiBold);
    p.setFont(big);
    p.setPen(Qt::white);
    p.drawText(circle, Qt::AlignCenter, initials.isEmpty() ? QStringLiteral("?") : initials);

    p.setFont(font());
    p.setPen(QColor(255, 255, 255, 225));
    p.drawText(QRect(0, 84, width(), height() - 84), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_peer.name);
}

void FinderPeerTile::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        m_hot = true;
        update();
    }
}

void FinderPeerTile::dragLeaveEvent(QDragLeaveEvent *)
{
    m_hot = false;
    update();
}

void FinderPeerTile::dropEvent(QDropEvent *event)
{
    m_hot = false;
    update();
    QList<QUrl> files;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            files.append(url);
        }
    }
    if (!files.isEmpty()) {
        event->acceptProposedAction();
        m_onFiles(files);
    }
}

void FinderPeerTile::mouseReleaseEvent(QMouseEvent *)
{
    m_onFiles({});
}

void FinderPeerTile::enterEvent(QEnterEvent *)
{
    m_hot = true;
    update();
}

void FinderPeerTile::leaveEvent(QEvent *)
{
    m_hot = false;
    update();
}

#include "moc_finderairdropservice.cpp"
