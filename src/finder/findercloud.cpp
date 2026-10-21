/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findercloud.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrlQuery>

namespace
{
const QString Remote = QStringLiteral("gdrive");
const QString Unit = QStringLiteral("finder-google-drive.service");

QString configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/finder/rclone.conf");
}

QString unitPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/systemd/user/") + Unit;
}

QString cachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/finder/google-drive");
}

QString runtimeDir()
{
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return dir.isEmpty() ? QDir::tempPath() : dir;
}

// What rclone logs when Google no longer accepts the token.
const QRegularExpression &signedOutPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("invalid_grant|token expired|couldn't fetch token|empty token|unauthorized_client|Token has been expired or revoked|Invalid Credentials|authError"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

QString socketPath()
{
    return runtimeDir() + QStringLiteral("/finder-google-drive.sock");
}

// The token rclone stores, from the [gdrive] section of Finder's config.
QByteArray storedToken()
{
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    bool inRemote = false;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith('[')) {
            inRemote = line == '[' + Remote.toLatin1() + ']';
        } else if (inRemote && line.startsWith("token")) {
            const int eq = line.indexOf('=');
            return eq < 0 ? QByteArray() : line.mid(eq + 1).trimmed();
        }
    }
    return {};
}

// `rclone authorize` prints the token as JSON, or for some backends as a
// base64 blob wrapping it.
QByteArray tokenFromAuthorize(const QByteArray &out)
{
    const int start = out.indexOf("--->");
    const int end = out.indexOf("<---End paste");
    if (start < 0 || end < start) {
        return {};
    }
    const QByteArray blob = out.mid(start + 4, end - start - 4).trimmed();
    for (const QByteArray &candidate : {blob, QByteArray::fromBase64(blob)}) {
        const QJsonObject object = QJsonDocument::fromJson(candidate).object();
        if (object.contains(QLatin1String("access_token"))) {
            return QJsonDocument(object).toJson(QJsonDocument::Compact);
        }
        if (object.value(QLatin1String("token")).isString()) {
            return object.value(QLatin1String("token")).toString().toUtf8();
        }
    }
    return {};
}
}

FinderCloud *FinderCloud::instance()
{
    static FinderCloud *cloud = new FinderCloud;
    return cloud;
}

FinderCloud::FinderCloud()
{
    if (hasAccount()) {
        m_state = Starting;
        // Keep the service in step with this build (say, a new rclone path);
        // a running mount picks it up at the next login.
        if (writeUnit()) {
            systemctl({QStringLiteral("daemon-reload")});
        }
    }
    followLog();
    m_pollTimer.setInterval(2000);
    connect(&m_pollTimer, &QTimer::timeout, this, &FinderCloud::poll);
    m_pollTimer.start();
    QTimer::singleShot(0, this, &FinderCloud::poll);
}

QString FinderCloud::mountPath()
{
    return QDir::homePath() + QStringLiteral("/Google Drive");
}

QUrl FinderCloud::webUrl()
{
    return QUrl(QStringLiteral("https://drive.google.com/"));
}

QString FinderCloud::rclone() const
{
#ifdef FINDER_RCLONE
    // The copy built from 3rdparty/rclone and installed with Finder.
    if (QFileInfo(QStringLiteral(FINDER_RCLONE)).isExecutable()) {
        return QStringLiteral(FINDER_RCLONE);
    }
#endif
    return QStandardPaths::findExecutable(QStringLiteral("rclone"));
}

FinderCloud::State FinderCloud::state() const
{
    return m_state;
}

QString FinderCloud::errorText() const
{
    return m_error;
}

int FinderCloud::uploads() const
{
    return m_uploads;
}

int FinderCloud::downloads() const
{
    return m_downloads;
}

int FinderCloud::uploadErrors() const
{
    return m_uploadErrors;
}

bool FinderCloud::isBusy() const
{
    return m_state == Ready && (m_uploads > 0 || m_downloads > 0);
}

QString FinderCloud::activityText() const
{
    switch (m_state) {
    case NotConnected:
        return i18nc("@info Finder Google Drive", "Not connected");
    case SigningIn:
        return i18nc("@info Finder Google Drive", "Signing in…");
    case Starting:
        return i18nc("@info Finder Google Drive", "Connecting…");
    case SignedOut:
    case Failed:
        return m_error;
    case Ready:
        break;
    }
    if (m_uploads > 0) {
        return i18ncp("@info Finder Google Drive", "Uploading 1 file…", "Uploading %1 files…", m_uploads);
    }
    if (m_downloads > 0) {
        return i18ncp("@info Finder Google Drive", "Downloading 1 file…", "Downloading %1 files…", m_downloads);
    }
    if (m_uploadErrors > 0) {
        return i18ncp("@info Finder Google Drive", "1 file couldn't upload; retrying", "%1 files couldn't upload; retrying", m_uploadErrors);
    }
    return i18nc("@info Finder Google Drive", "Up to date");
}

void FinderCloud::setState(State state, const QString &error)
{
    if (state != Ready) {
        m_uploads = m_downloads = m_uploadErrors = 0;
    }
    if (m_state == state && m_error == error) {
        return;
    }
    m_state = state;
    m_error = error;
    Q_EMIT stateChanged(state);
    Q_EMIT activityChanged();
}

bool FinderCloud::hasAccount() const
{
    return storedToken().contains("refresh_token");
}

void FinderCloud::writeConfig(const QByteArray &token)
{
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    // Written beside the old file, owner-only from the start, then swapped in.
    const QString temp = configPath() + QStringLiteral(".new");
    QFile::remove(temp);
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return;
    }
    file.write("[" + Remote.toLatin1() + "]\ntype = drive\nscope = drive\ntoken = " + token + "\n");
    file.close();
    QFile::remove(configPath());
    QFile::rename(temp, configPath());
}

bool FinderCloud::writeUnit()
{
    // The rc socket sits in the user's runtime dir (mode 0700), so only this
    // user reaches it; --rc-no-auth lets Finder ask Google for the quota.
    const QString unit = QStringLiteral(
                             "# Written by Finder; it rewrites this file, so edits here don't last.\n"
                             "[Unit]\n"
                             "Description=Google Drive for Finder\n"
                             "After=network-online.target\n"
                             "Wants=network-online.target\n"
                             "# A revoked token fails every start; give up instead of retrying forever.\n"
                             "StartLimitIntervalSec=5min\n"
                             "StartLimitBurst=5\n"
                             "\n"
                             "[Service]\n"
                             "Type=notify\n"
                             "ExecStartPre=/usr/bin/mkdir -p \"%h/Google Drive\"\n"
                             "ExecStartPre=-/usr/bin/rm -f %t/finder-google-drive.sock\n"
                             "ExecStart=%1 mount %2: \"%h/Google Drive\" \\\n"
                             "    --config \"%3\" \\\n"
                             "    --cache-dir \"%C/finder/google-drive\" \\\n"
                             "    --volname \"Google Drive\" \\\n"
                             "    --vfs-cache-mode full \\\n"
                             "    --vfs-cache-max-size 10G \\\n"
                             "    --dir-cache-time 5m \\\n"
                             "    --poll-interval 1m \\\n"
                             "    --drive-export-formats docx,xlsx,pptx,svg \\\n"
                             "    --rc --rc-addr unix://%t/finder-google-drive.sock --rc-no-auth\n"
                             "ExecStop=/usr/bin/fusermount3 -uz \"%h/Google Drive\"\n"
                             "Restart=on-failure\n"
                             "RestartSec=10\n"
                             "\n"
                             "[Install]\n"
                             "WantedBy=default.target\n")
                             .arg(rclone(), Remote, configPath());
    QFile file(unitPath());
    if (file.open(QIODevice::ReadOnly) && file.readAll() == unit.toUtf8()) {
        return false;
    }
    file.close();
    QDir().mkpath(QFileInfo(unitPath()).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(unit.toUtf8());
    return true;
}

void FinderCloud::systemctl(const QStringList &args, std::function<void(bool ok)> done)
{
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (done) {
            done(status == QProcess::NormalExit && code == 0);
        }
    });
    process->start(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user")} + args);
}

void FinderCloud::signIn(std::function<void(bool ok, const QString &error)> done)
{
    if (m_authorize) {
        m_signInDone = done; // already waiting on the browser
        return;
    }
    const QString program = rclone();
    if (program.isEmpty()) {
        done(false, i18nc("@info Finder Google Drive", "Finder was built without rclone, and it isn't installed either."));
        return;
    }
    const State before = m_state;
    const QString beforeError = m_error;
    m_signInDone = done;
    m_signInUrl.clear();
    setState(SigningIn);

    auto *process = new QProcess(this);
    m_authorize = process;
    auto stderrText = std::make_shared<QByteArray>();
    connect(process, &QProcess::readyReadStandardError, this, [this, process, stderrText]() {
        const QByteArray chunk = process->readAllStandardError();
        stderrText->append(chunk);
        static const QRegularExpression link(QStringLiteral("(http://127\\.0\\.0\\.1:\\d+/auth\\?state=\\S+)"));
        const QRegularExpressionMatch match = link.match(QString::fromUtf8(*stderrText));
        if (match.hasMatch() && m_signInUrl.isEmpty()) {
            m_signInUrl = QUrl(match.captured(1));
            QDesktopServices::openUrl(m_signInUrl);
            Q_EMIT signInUrlChanged(m_signInUrl);
        }
    });
    connect(process, &QProcess::finished, this, [this, process, stderrText, before, beforeError](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_signInUrl.clear();
        auto done = std::exchange(m_signInDone, {});
        const QByteArray token = status == QProcess::NormalExit && code == 0 ? tokenFromAuthorize(process->readAllStandardOutput()) : QByteArray();
        if (token.isEmpty()) {
            setState(before, beforeError);
            QString error;
            if (status == QProcess::NormalExit && process->property("cancelled").isNull()) {
                const QString log = QString::fromUtf8(*stderrText);
                if (log.contains(QLatin1String("access_denied"))) {
                    error = i18nc("@info Finder Google Drive", "Google Drive wasn't connected because access wasn't allowed.");
                } else {
                    // rclone explains auth errors on a "Description:" line.
                    static const QRegularExpression description(QStringLiteral("^Description: (.+)$"), QRegularExpression::MultilineOption);
                    error = description.match(log).captured(1).trimmed();
                    if (error.isEmpty()) {
                        error = i18nc("@info Finder Google Drive", "Google didn't finish signing you in.");
                    }
                }
            }
            if (done) {
                done(false, error);
            }
            return;
        }
        writeConfig(token);
        writeUnit();
        setState(Starting);
        followLog();
        systemctl({QStringLiteral("daemon-reload")}, [this](bool) {
            // restart, not start: a mount still running on an expired token
            // has to pick up the new one.
            systemctl({QStringLiteral("reset-failed"), Unit}, [this](bool) { // past its start limit
                systemctl({QStringLiteral("enable"), Unit}, [this](bool) {
                    systemctl({QStringLiteral("restart"), Unit});
                });
            });
        });
        if (done) {
            done(true, {});
        }
    });
    // Finder opens the page itself, and shows its own page when Google is done.
    // --auto-confirm takes the default for rclone's follow-up questions (not a
    // Shared Drive) instead of waiting on the terminal.
    process->start(program,
                   {QStringLiteral("authorize"), QStringLiteral("drive"), QStringLiteral("--auth-no-open-browser"), QStringLiteral("--auto-confirm"),
                    QStringLiteral("--config"), configPath(), QStringLiteral("--template"), authTemplate()});
    process->closeWriteChannel();
}

void FinderCloud::cancelSignIn()
{
    if (m_authorize) {
        m_authorize->setProperty("cancelled", true);
        m_authorize->kill();
    }
}

QUrl FinderCloud::signInUrl() const
{
    return m_signInUrl;
}

void FinderCloud::start()
{
    if (!hasAccount() || m_state == Ready) {
        return;
    }
    writeUnit();
    systemctl({QStringLiteral("daemon-reload")}, [this](bool) {
        systemctl({QStringLiteral("reset-failed"), Unit}, [this](bool) { // past its start limit
            systemctl({QStringLiteral("enable"), QStringLiteral("--now"), Unit});
        });
    });
    setState(Starting);
}

void FinderCloud::disconnect()
{
    cancelSignIn();
    const QByteArray token = storedToken();
    if (m_journal) {
        m_journal->kill();
    }
    setState(NotConnected);
    systemctl({QStringLiteral("disable"), QStringLiteral("--now"), Unit}, [this](bool) {
        QFile::remove(unitPath());
        systemctl({QStringLiteral("daemon-reload")});
        QFile::remove(configPath());
        QDir(cachePath()).removeRecursively();
        QDir().rmdir(mountPath()); // only if it's empty, i.e. unmounted
    });

    // Tell Google to forget the token too, so it can't be used again.
    const QString refresh = QJsonDocument::fromJson(token).object().value(QLatin1String("refresh_token")).toString();
    if (!refresh.isEmpty()) {
        auto *network = new QNetworkAccessManager(this);
        QNetworkRequest request(QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("token"), refresh);
        QNetworkReply *reply = network->post(request, form.query(QUrl::FullyEncoded).toUtf8());
        connect(reply, &QNetworkReply::finished, network, &QObject::deleteLater);
    }
}

void FinderCloud::signedOut()
{
    // No point keeping a mount (or retrying every 10 s) with a dead token;
    // edits not yet uploaded stay in the cache and go up after signing in.
    systemctl({QStringLiteral("stop"), Unit});
    setState(SignedOut, i18nc("@info Finder Google Drive", "Google signed you out. Sign in again to keep syncing."));
}

void FinderCloud::followLog()
{
    if (m_journal || !hasAccount()) {
        return;
    }
    // A running mount doesn't exit when Google revokes its token; it only
    // logs failed requests (the change poll makes one each minute).
    auto *journal = new QProcess(this);
    m_journal = journal;
    connect(journal, &QProcess::readyReadStandardOutput, this, [this, journal]() {
        while (journal->canReadLine()) {
            const QString line = QString::fromUtf8(journal->readLine());
            if (m_state != SignedOut && m_state != SigningIn && signedOutPattern().match(line).hasMatch()) {
                signedOut();
            }
        }
    });
    connect(journal, &QProcess::finished, journal, &QObject::deleteLater);
    journal->start(QStringLiteral("journalctl"),
                   {QStringLiteral("--user"), QStringLiteral("-u"), Unit, QStringLiteral("-f"), QStringLiteral("-n"), QStringLiteral("0"), QStringLiteral("-o"),
                    QStringLiteral("cat")});
}

void FinderCloud::poll()
{
    // Signed out stays so until the user signs in again (the service is
    // stopped, and may still be unmounting).
    if (m_state == SigningIn || (m_state == SignedOut && hasAccount())) {
        return;
    }
    const bool account = hasAccount();
    if (!account) {
        if (m_state != NotConnected) {
            setState(NotConnected); // disconnected from another window
        }
        return;
    }
    const QStorageInfo storage(mountPath());
    const bool mounted = storage.isValid() && storage.rootPath() == mountPath() && storage.fileSystemType().startsWith("fuse.rclone");
    if (mounted) {
        setState(Ready);
        requestStats();
        return;
    }
    if (m_state == NotConnected || m_state == Ready) {
        setState(Starting);
    }
    checkService();
}

void FinderCloud::checkService()
{
    if (m_checking) {
        return;
    }
    // Signed out or failed: look again only now and then.
    static int skipped = 0;
    if ((m_state == SignedOut || m_state == Failed) && ++skipped % 5 != 0) {
        return;
    }
    m_checking = true;
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process]() {
        process->deleteLater();
        QHash<QString, QString> props;
        for (const QString &line : QString::fromUtf8(process->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            props.insert(line.section(QLatin1Char('='), 0, 0), line.section(QLatin1Char('='), 1));
        }
        const QString active = props.value(QStringLiteral("ActiveState"));
        const QString sub = props.value(QStringLiteral("SubState"));
        if (active == QLatin1String("active") || (active == QLatin1String("activating") && sub != QLatin1String("auto-restart"))) {
            m_checking = false;
            if (m_state != Ready) {
                setState(Starting); // mounting
            }
            return;
        }
        if (active == QLatin1String("inactive") && props.value(QStringLiteral("Result")) == QLatin1String("success")) {
            // Never started, or stopped by hand: start it.
            m_checking = false;
            if (m_state == Starting) {
                start();
            }
            return;
        }
        // Failed, or failing and about to retry: the log says why.
        auto *journal = new QProcess(this);
        connect(journal, &QProcess::finished, this, [this, journal]() {
            journal->deleteLater();
            m_checking = false;
            const QString log = QString::fromUtf8(journal->readAllStandardOutput());
            if (signedOutPattern().match(log).hasMatch()) {
                signedOut();
                return;
            }
            QString reason;
            for (const QString &line : log.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (line.contains(QLatin1String("ERROR")) || line.contains(QLatin1String("CRITICAL")) || line.startsWith(QLatin1String("Fatal"))) {
                    reason = line.section(QStringLiteral(": "), 1).trimmed();
                }
            }
            setState(Failed, reason.isEmpty() ? i18nc("@info Finder Google Drive", "Google Drive couldn't start.") : reason);
        });
        journal->start(QStringLiteral("journalctl"), {QStringLiteral("--user"), QStringLiteral("-u"), Unit, QStringLiteral("-n"), QStringLiteral("30"), QStringLiteral("-o"), QStringLiteral("cat"), QStringLiteral("--no-pager")});
    });
    process->start(QStringLiteral("systemctl"),
                   {QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("-p"), QStringLiteral("ActiveState"), QStringLiteral("-p"),
                    QStringLiteral("SubState"), QStringLiteral("-p"), QStringLiteral("Result"), Unit});
}

namespace
{
// One call to rclone's remote control API: HTTP/1.0 over the unix socket, so
// the reply comes back whole, without chunking, and the socket then closes.
void rcCall(QObject *context, const QString &method, std::function<void(const QJsonObject &, const QByteArray &)> done, const QByteArray &params = "{}")
{
    auto *socket = new QLocalSocket(context);
    auto finish = [socket, done]() {
        if (socket->property("finished").toBool()) {
            return;
        }
        socket->setProperty("finished", true);
        const QByteArray reply = socket->property("reply").toByteArray() + socket->readAll();
        const int body = reply.indexOf("\r\n\r\n");
        const bool ok = reply.startsWith("HTTP/1.") && reply.left(reply.indexOf('\n')).contains(" 200 ") && body > 0;
        done(ok ? QJsonDocument::fromJson(reply.mid(body + 4)).object() : QJsonObject(), body > 0 ? reply.mid(body + 4) : QByteArray());
        socket->deleteLater();
    };
    QObject::connect(socket, &QLocalSocket::connected, socket, [socket, method, params]() {
        socket->write("POST /" + method.toLatin1() + " HTTP/1.0\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(params.size()) + "\r\n\r\n" + params);
    });
    QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket]() {
        socket->setProperty("reply", QByteArray(socket->property("reply").toByteArray() + socket->readAll()));
    });
    QObject::connect(socket, &QLocalSocket::disconnected, socket, finish);
    QObject::connect(socket, &QLocalSocket::errorOccurred, socket, finish);
    QTimer::singleShot(3000, socket, finish);
    socket->connectToServer(socketPath());
}
}

void FinderCloud::requestStats()
{
    if (m_statsPending) {
        return;
    }
    m_statsPending = true;
    // Once a minute, ask Google itself (for the quota): a mount whose token
    // was revoked only fails when something touches the drive.
    static int polls = 0;
    if (++polls % 30 == 1) {
        rcCall(
            this, QStringLiteral("operations/about"),
            [this](const QJsonObject &, const QByteArray &raw) {
                if (m_state == Ready && signedOutPattern().match(QString::fromUtf8(raw)).hasMatch()) {
                    signedOut();
                }
            },
            "{\"fs\":\"" + Remote.toLatin1() + ":\"}");
    }
    rcCall(this, QStringLiteral("vfs/stats"), [this](const QJsonObject &vfs, const QByteArray &) {
        rcCall(this, QStringLiteral("core/stats"), [this, vfs](const QJsonObject &core, const QByteArray &) {
            m_statsPending = false;
            if (m_state != Ready) {
                return;
            }
            const QJsonObject cache = vfs.value(QLatin1String("diskCache")).toObject();
            const int uploading = cache.value(QLatin1String("uploadsInProgress")).toInt();
            const int uploads = uploading + cache.value(QLatin1String("uploadsQueued")).toInt();
            const int errors = cache.value(QLatin1String("erroredFiles")).toInt();
            // core/stats counts uploads among its transfers too.
            const int downloads = qMax(0, int(core.value(QLatin1String("transferring")).toArray().size()) - uploading);
            if (uploads != m_uploads || downloads != m_downloads || errors != m_uploadErrors) {
                m_uploads = uploads;
                m_downloads = downloads;
                m_uploadErrors = errors;
                Q_EMIT activityChanged();
            }
        });
    });
}

QString FinderCloud::authTemplate()
{
    // The page Google sends the browser back to; a Go template for rclone.
    const QString path = runtimeDir() + QStringLiteral("/finder-google-signin.html");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{ if .OK }}Google Drive is connected{{ else }}Couldn't connect Google Drive{{ end }}</title>
<style>
:root { color-scheme: light dark; --bg: #f5f5f7; --card: #fff; --text: #1d1d1f; --dim: #6e6e73; --accent: #0071e3; }
@media (prefers-color-scheme: dark) { :root { --bg: #1c1c1e; --card: #2c2c2e; --text: #f5f5f7; --dim: #a1a1a6; --accent: #0a84ff; } }
body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: var(--bg); color: var(--text);
       font: 15px/1.45 -apple-system, "SF Pro Text", Inter, "Noto Sans", sans-serif; }
main { background: var(--card); border-radius: 16px; padding: 36px 40px; max-width: 420px; margin: 16px; text-align: center;
       box-shadow: 0 10px 40px rgba(0,0,0,.12); }
.mark { width: 56px; height: 56px; border-radius: 50%; margin: 0 auto 16px; display: grid; place-items: center; color: #fff;
        font-size: 30px; background: {{ if .OK }}#34c759{{ else }}#ff9f0a{{ end }}; }
h1 { font-size: 20px; margin: 0 0 8px; }
p { margin: 0; color: var(--dim); }
code { font-size: 13px; }
</style></head>
<body><main>
{{ if .OK }}
<div class="mark">&#10003;</div>
<h1>Google Drive is connected</h1>
<p>You can close this tab. Google Drive is in Finder's sidebar under Locations.</p>
{{ else }}
<div class="mark">!</div>
<h1>Couldn't connect Google Drive</h1>
<p>{{ .Name }}{{ if .Description }}: {{ .Description }}{{ end }}</p>
<p>Close this tab and try again from Finder.</p>
{{ end }}
</main></body></html>
)");
    }
    return path;
}

#include "moc_findercloud.cpp"
