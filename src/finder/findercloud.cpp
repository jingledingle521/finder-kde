/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findercloud.h"

#include <KLocalizedString>

#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
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
QString runtimeDir()
{
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return dir.isEmpty() ? QDir::tempPath() : dir;
}

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
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

// rclone's complaint from a failed command, without its "Error:" or log prefix.
QString rcloneError(const QByteArray &stderrText)
{
    QString error;
    for (const QString &line : QString::fromUtf8(stderrText).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (line.startsWith(QLatin1String("Error: "))) {
            error = line.mid(7).trimmed();
        } else if (line.contains(QLatin1String("CRITICAL")) || line.contains(QLatin1String("Fatal error"))) {
            error = line.section(QStringLiteral(": "), 1).trimmed();
        }
    }
    return error;
}
}

FinderCloud *FinderCloud::instance(Provider provider)
{
    static FinderCloud *clouds[] = {new FinderCloud(GoogleDrive), new FinderCloud(ICloudDrive)};
    return clouds[provider];
}

QList<FinderCloud *> FinderCloud::all()
{
    // In sidebar order: iCloud first, as on macOS.
    return {instance(ICloudDrive), instance(GoogleDrive)};
}

FinderCloud::FinderCloud(Provider provider)
    : m_provider(provider)
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

FinderCloud::Provider FinderCloud::provider() const
{
    return m_provider;
}

QString FinderCloud::name() const
{
    return m_provider == GoogleDrive ? i18nc("@item Finder cloud drive", "Google Drive") : i18nc("@item Finder cloud drive", "iCloud Drive");
}

QString FinderCloud::mountPath() const
{
    // Not translated: it's a folder name on disk, and the service names it too.
    return QDir::homePath() + (m_provider == GoogleDrive ? QStringLiteral("/Google Drive") : QStringLiteral("/iCloud Drive"));
}

QUrl FinderCloud::webUrl() const
{
    return QUrl(m_provider == GoogleDrive ? QStringLiteral("https://drive.google.com/") : QStringLiteral("https://www.icloud.com/iclouddrive/"));
}

QString FinderCloud::iconName() const
{
    return m_provider == GoogleDrive ? QStringLiteral("folder-google-drive") : QStringLiteral("icloud");
}

QString FinderCloud::symbolicIconName() const
{
    const QStringList names = m_provider == GoogleDrive
        ? QStringList{QStringLiteral("folder-google-drive-symbolic"), QStringLiteral("folder-cloud-symbolic")}
        : QStringList{QStringLiteral("icloud-symbolic"), QStringLiteral("folder-cloud-symbolic"), QStringLiteral("weather-overcast-symbolic")};
    for (const QString &name : names) {
        if (QIcon::hasThemeIcon(name)) {
            return name;
        }
    }
    return QStringLiteral("folder-symbolic");
}

QString FinderCloud::remote() const
{
    return m_provider == GoogleDrive ? QStringLiteral("gdrive") : QStringLiteral("icloud");
}

QString FinderCloud::unit() const
{
    return m_provider == GoogleDrive ? QStringLiteral("finder-google-drive.service") : QStringLiteral("finder-icloud-drive.service");
}

QString FinderCloud::configPath() const
{
    // One file per drive, so signing in to one never rewrites the other.
    return configDir() + (m_provider == GoogleDrive ? QStringLiteral("/finder/rclone.conf") : QStringLiteral("/finder/rclone-icloud.conf"));
}

QString FinderCloud::unitPath() const
{
    return configDir() + QStringLiteral("/systemd/user/") + unit();
}

QString FinderCloud::cachePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + (m_provider == GoogleDrive ? QStringLiteral("/finder/google-drive") : QStringLiteral("/finder/icloud-drive"));
}

QString FinderCloud::socketPath() const
{
    return runtimeDir() + QLatin1Char('/') + QString(unit()).replace(QStringLiteral(".service"), QStringLiteral(".sock"));
}

QString FinderCloud::rclone()
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
        return i18nc("@info Finder cloud drive", "Not connected");
    case SigningIn:
        return i18nc("@info Finder cloud drive", "Signing in…");
    case Starting:
        return i18nc("@info Finder cloud drive", "Connecting…");
    case SignedOut:
    case Failed:
        return m_error;
    case Ready:
        break;
    }
    if (m_uploads > 0) {
        return i18ncp("@info Finder cloud drive", "Uploading 1 file…", "Uploading %1 files…", m_uploads);
    }
    if (m_downloads > 0) {
        return i18ncp("@info Finder cloud drive", "Downloading 1 file…", "Downloading %1 files…", m_downloads);
    }
    if (m_uploadErrors > 0) {
        return i18ncp("@info Finder cloud drive", "1 file couldn't upload; retrying", "%1 files couldn't upload; retrying", m_uploadErrors);
    }
    return i18nc("@info Finder cloud drive", "Up to date");
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

QByteArray FinderCloud::configValue(const QByteArray &key) const
{
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    bool inRemote = false;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith('[')) {
            inRemote = line == '[' + remote().toLatin1() + ']';
        } else if (const int eq = line.indexOf('='); inRemote && eq > 0 && line.left(eq).trimmed() == key) {
            return line.mid(eq + 1).trimmed();
        }
    }
    return {};
}

QString FinderCloud::accountName() const
{
    return QString::fromUtf8(configValue("apple_id"));
}

bool FinderCloud::hasAccount() const
{
    // Google keeps an OAuth token; iCloud a trust token once 2FA is done.
    return m_provider == GoogleDrive ? configValue("token").contains("refresh_token") : !configValue("trust_token").isEmpty();
}

bool FinderCloud::matchesSignedOut(const QString &log) const
{
    // What rclone logs when the provider no longer accepts the credentials.
    static const QRegularExpression google(
        QStringLiteral("invalid_grant|token expired|couldn't fetch token|empty token|unauthorized_client|Token has been expired or revoked|Invalid Credentials|authError"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression icloud(QStringLiteral("trust token expired|missing icloud trust token|please reauth|incorrect username or password"),
                                           QRegularExpression::CaseInsensitiveOption);
    return (m_provider == GoogleDrive ? google : icloud).match(log).hasMatch();
}

QString FinderCloud::signedOutText() const
{
    return m_provider == GoogleDrive ? i18nc("@info Finder cloud drive", "Google signed you out. Sign in again to keep syncing.")
                                     : i18nc("@info Finder cloud drive", "Your iCloud sign-in expired. Sign in again to keep syncing.");
}

void FinderCloud::writeGoogleConfig(const QByteArray &token)
{
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    // Written beside the old file, owner-only from the start, then swapped in.
    const QString temp = configPath() + QStringLiteral(".new");
    QFile::remove(temp);
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return;
    }
    file.write("[" + remote().toLatin1() + "]\ntype = drive\nscope = drive\ntoken = " + token + "\n");
    file.close();
    QFile::remove(configPath());
    QFile::rename(temp, configPath());
}

bool FinderCloud::writeUnit()
{
    const QString folder = QFileInfo(mountPath()).fileName();
    const QString socket = QFileInfo(socketPath()).fileName();
    const QString cache = cachePath().section(QLatin1Char('/'), -2); // finder/<drive>
    // Google exports Docs, Sheets and Slides as Office files; iCloud has no
    // such documents, and no change feed to poll.
    const QString extra = m_provider == GoogleDrive ? QStringLiteral("    --poll-interval 1m \\\n    --drive-export-formats docx,xlsx,pptx,svg \\\n") : QString();
    // The rc socket sits in the user's runtime dir (mode 0700), so only this
    // user reaches it; --rc-no-auth lets Finder ask the provider for the quota.
    const QString text = QStringLiteral(
                             "# Written by Finder; it rewrites this file, so edits here don't last.\n"
                             "[Unit]\n"
                             "Description=%5 for Finder\n"
                             "After=network-online.target\n"
                             "Wants=network-online.target\n"
                             "# Expired credentials fail every start; give up instead of retrying forever.\n"
                             "StartLimitIntervalSec=5min\n"
                             "StartLimitBurst=5\n"
                             "\n"
                             "[Service]\n"
                             "Type=notify\n"
                             "ExecStartPre=/usr/bin/mkdir -p \"%h/%6\"\n"
                             "ExecStartPre=-/usr/bin/rm -f %t/%7\n"
                             "ExecStart=%1 mount %2: \"%h/%6\" \\\n"
                             "    --config \"%3\" \\\n"
                             "    --cache-dir \"%C/%8\" \\\n"
                             "    --volname \"%6\" \\\n"
                             "    --vfs-cache-mode full \\\n"
                             "    --vfs-cache-max-size 10G \\\n"
                             "    --dir-cache-time 5m \\\n"
                             "%4"
                             "    --rc --rc-addr unix://%t/%7 --rc-no-auth\n"
                             "ExecStop=/usr/bin/fusermount3 -uz \"%h/%6\"\n"
                             "Restart=on-failure\n"
                             "RestartSec=10\n"
                             "\n"
                             "[Install]\n"
                             "WantedBy=default.target\n")
                             .arg(rclone(), remote(), configPath(), extra, name(), folder, socket, cache);
    QFile file(unitPath());
    if (file.open(QIODevice::ReadOnly) && file.readAll() == text.toUtf8()) {
        return false;
    }
    file.close();
    QDir().mkpath(QFileInfo(unitPath()).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(text.toUtf8());
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

void FinderCloud::finishSignIn()
{
    writeUnit();
    setState(Starting);
    followLog();
    systemctl({QStringLiteral("daemon-reload")}, [this](bool) {
        // restart, not start: a mount still running on expired credentials
        // has to pick up the new ones.
        systemctl({QStringLiteral("reset-failed"), unit()}, [this](bool) { // past its start limit
            systemctl({QStringLiteral("enable"), unit()}, [this](bool) {
                systemctl({QStringLiteral("restart"), unit()});
            });
        });
    });
}

void FinderCloud::signIn(std::function<void(bool ok, const QString &error)> done)
{
    Q_ASSERT(m_provider == GoogleDrive);
    if (m_signInProcess) {
        m_signInDone = done; // already waiting on the browser
        return;
    }
    const QString program = rclone();
    if (program.isEmpty()) {
        done(false, i18nc("@info Finder cloud drive", "Finder was built without rclone, and it isn't installed either."));
        return;
    }
    m_stateBeforeSignIn = m_state;
    m_errorBeforeSignIn = m_error;
    m_signInDone = done;
    m_signInUrl.clear();
    setState(SigningIn);

    auto *process = new QProcess(this);
    m_signInProcess = process;
    auto stderrText = std::make_shared<QByteArray>();
    connect(process, &QProcess::readyReadStandardError, this, [this, process, stderrText]() {
        stderrText->append(process->readAllStandardError());
        static const QRegularExpression link(QStringLiteral("(http://127\\.0\\.0\\.1:\\d+/auth\\?state=\\S+)"));
        const QRegularExpressionMatch match = link.match(QString::fromUtf8(*stderrText));
        if (match.hasMatch() && m_signInUrl.isEmpty()) {
            m_signInUrl = QUrl(match.captured(1));
            QDesktopServices::openUrl(m_signInUrl);
            Q_EMIT signInUrlChanged(m_signInUrl);
        }
    });
    connect(process, &QProcess::finished, this, [this, process, stderrText](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_signInUrl.clear();
        auto done = std::exchange(m_signInDone, {});
        const QByteArray token = status == QProcess::NormalExit && code == 0 ? tokenFromAuthorize(process->readAllStandardOutput()) : QByteArray();
        if (token.isEmpty()) {
            setState(m_stateBeforeSignIn, m_errorBeforeSignIn);
            QString error;
            if (status == QProcess::NormalExit && process->property("cancelled").isNull()) {
                const QString log = QString::fromUtf8(*stderrText);
                if (log.contains(QLatin1String("access_denied"))) {
                    error = i18nc("@info Finder cloud drive", "Google Drive wasn't connected because access wasn't allowed.");
                } else {
                    // rclone explains auth errors on a "Description:" line.
                    static const QRegularExpression description(QStringLiteral("^Description: (.+)$"), QRegularExpression::MultilineOption);
                    error = description.match(log).captured(1).trimmed();
                    if (error.isEmpty()) {
                        error = i18nc("@info Finder cloud drive", "Google didn't finish signing you in.");
                    }
                }
            }
            if (done) {
                done(false, error);
            }
            return;
        }
        writeGoogleConfig(token);
        finishSignIn();
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

QUrl FinderCloud::signInUrl() const
{
    return m_signInUrl;
}

void FinderCloud::signInWithPassword(const QString &appleId, const QString &password, std::function<void(const Step &)> next)
{
    Q_ASSERT(m_provider == ICloudDrive);
    const QString program = rclone();
    if (program.isEmpty()) {
        next({Step::Error, {}, {}, i18nc("@info Finder cloud drive", "Finder was built without rclone, and it isn't installed either.")});
        return;
    }
    if (m_state != SigningIn) {
        m_stateBeforeSignIn = m_state;
        m_errorBeforeSignIn = m_error;
        setState(SigningIn);
    }
    // The password goes through `rclone obscure` on stdin, so it never shows
    // up in the process list; rclone derives Apple's SRP proof from it locally.
    auto *obscure = new QProcess(this);
    m_signInProcess = obscure;
    connect(obscure, &QProcess::finished, this, [this, obscure, appleId, next](int code, QProcess::ExitStatus status) {
        obscure->deleteLater();
        const QString obscured = QString::fromUtf8(obscure->readAllStandardOutput()).trimmed();
        if (obscure->property("cancelled").toBool()) {
            return;
        }
        if (status != QProcess::NormalExit || code != 0 || obscured.isEmpty()) {
            abandonSignIn();
            next({Step::Error, {}, {}, rcloneError(obscure->readAllStandardError())});
            return;
        }
        QDir().mkpath(QFileInfo(configPath()).absolutePath());
        if (!QFileInfo::exists(configPath())) {
            // Owner-only before rclone writes anything into it.
            QFile file(configPath());
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                abandonSignIn();
                next({Step::Error, {}, {}, file.errorString()});
                return;
            }
        }
        runConfigStep({QStringLiteral("config"), QStringLiteral("create"), remote(), QStringLiteral("iclouddrive"), QStringLiteral("apple_id=") + appleId,
                       QStringLiteral("password=") + obscured, QStringLiteral("--no-obscure")},
                      next);
    });
    obscure->start(program, {QStringLiteral("obscure"), QStringLiteral("-")});
    obscure->write(password.toUtf8());
    obscure->closeWriteChannel();
}

void FinderCloud::answer(const QString &result, std::function<void(const Step &)> next)
{
    runConfigStep({QStringLiteral("config"), QStringLiteral("update"), remote(), QStringLiteral("--continue"), QStringLiteral("--state"), m_configState,
                   QStringLiteral("--result"), result},
                  next);
}

void FinderCloud::runConfigStep(const QStringList &args, std::function<void(const Step &)> next)
{
    // rclone's non-interactive config: each call answers one question and
    // prints the next as JSON ({State, Option, Error}); no State means done.
    auto *process = new QProcess(this);
    m_signInProcess = process;
    connect(process, &QProcess::finished, this, [this, process, next](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (process->property("cancelled").toBool()) {
            return;
        }
        const QJsonObject out = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        if (status != QProcess::NormalExit || code != 0 || out.isEmpty()) {
            const QByteArray stderrText = process->readAllStandardError();
            // The whole story goes to the journal (journalctl --user -t finder);
            // the sheet only shows the gist. The password is never in it.
            qWarning().noquote() << "Finder: iCloud sign-in failed (rclone exit" << code << "):" << QString::fromUtf8(stderrText).trimmed();
            QString error = rcloneError(stderrText);
            // Apple's own reason, when rclone passes it on (see
            // 3rdparty/rclone-patches): e.g. a locked account, not a typo.
            QString apple;
            if (const int said = stderrText.indexOf("Apple said: "); said >= 0) {
                const QByteArray reply = stderrText.mid(said + 12);
                const int start = reply.indexOf('{');
                const int end = reply.lastIndexOf('}');
                const QJsonObject body = QJsonDocument::fromJson(reply.mid(start, end - start + 1)).object();
                const QJsonArray errors = body.value(QLatin1String("serviceErrors")).toArray();
                if (!errors.isEmpty()) {
                    apple = errors.at(0).toObject().value(QLatin1String("message")).toString();
                }
            }
            if (!apple.isEmpty()) {
                error = apple;
            } else if (error.contains(QLatin1String("incorrect username or password"), Qt::CaseInsensitive)) {
                error = i18nc("@info Finder cloud drive", "Your Apple ID or password is incorrect.");
            } else if (error.contains(QLatin1String("validate2FACode")) || error.contains(QLatin1String("validateSMSCode"))) {
                error = i18nc("@info Finder cloud drive", "That code didn't work. Check it and try again, or sign in again for a new one.");
            } else if (error.isEmpty()) {
                error = i18nc("@info Finder cloud drive", "Apple didn't finish signing you in.");
            }
            abandonSignIn();
            next({Step::Error, {}, {}, error});
            return;
        }
        m_configState = out.value(QLatin1String("State")).toString();
        const QJsonObject option = out.value(QLatin1String("Option")).toObject();
        if (m_configState.isEmpty() && option.isEmpty()) {
            if (!hasAccount()) {
                abandonSignIn();
                next({Step::Error, {}, {}, i18nc("@info Finder cloud drive", "Apple didn't hand back a sign-in. Try again.")});
                return;
            }
            finishSignIn();
            next({Step::Done, {}, {}, {}});
            return;
        }
        const QString error = out.value(QLatin1String("Error")).toString();
        if (option.isEmpty()) {
            // An error that sends rclone back a step (e.g. a blank code): go on
            // to the question it asks again, and show the error with it.
            runConfigStep({QStringLiteral("config"), QStringLiteral("update"), remote(), QStringLiteral("--continue"), QStringLiteral("--state"), m_configState,
                           QStringLiteral("--result"), QString()},
                          [next, error](Step step) {
                              if (step.message.isEmpty()) {
                                  step.message = error;
                              }
                              next(step);
                          });
            return;
        }
        Step step;
        step.prompt = option.value(QLatin1String("Help")).toString().section(QLatin1Char('\n'), 0, 0);
        step.message = error;
        const QJsonArray examples = option.value(QLatin1String("Examples")).toArray();
        if (!examples.isEmpty()) {
            step.kind = Step::Choice;
            for (const QJsonValue &example : examples) {
                const QJsonObject e = example.toObject();
                step.choices.append({e.value(QLatin1String("Value")).toString(), e.value(QLatin1String("Help")).toString()});
            }
        } else {
            step.kind = Step::Code;
        }
        next(step);
    });
    process->start(rclone(), args + QStringList{QStringLiteral("--non-interactive"), QStringLiteral("--config"), configPath()});
    process->closeWriteChannel();
}

void FinderCloud::abandonSignIn()
{
    m_configState.clear();
    if (m_stateBeforeSignIn == NotConnected && !hasAccount()) {
        // A first sign-in that didn't finish: leave nothing behind (the
        // half-made remote holds the obscured password).
        QFile::remove(configPath());
    }
    if (m_state == SigningIn) {
        setState(m_stateBeforeSignIn, m_errorBeforeSignIn);
    }
}

void FinderCloud::cancelSignIn()
{
    if (m_signInProcess) {
        m_signInProcess->setProperty("cancelled", true);
        m_signInProcess->kill();
    }
    if (m_provider == ICloudDrive) {
        abandonSignIn();
    }
}

void FinderCloud::start()
{
    if (!hasAccount() || m_state == Ready) {
        return;
    }
    writeUnit();
    systemctl({QStringLiteral("daemon-reload")}, [this](bool) {
        systemctl({QStringLiteral("reset-failed"), unit()}, [this](bool) { // past its start limit
            systemctl({QStringLiteral("enable"), QStringLiteral("--now"), unit()});
        });
    });
    setState(Starting);
}

void FinderCloud::disconnect()
{
    cancelSignIn();
    const QByteArray token = configValue("token");
    if (m_journal) {
        m_journal->kill();
    }
    setState(NotConnected);
    systemctl({QStringLiteral("disable"), QStringLiteral("--now"), unit()}, [this](bool) {
        QFile::remove(unitPath());
        systemctl({QStringLiteral("daemon-reload")});
        QFile::remove(configPath());
        QDir(cachePath()).removeRecursively();
        QDir().rmdir(mountPath()); // only if it's empty, i.e. unmounted
    });

    // Tell Google to forget the token too, so it can't be used again. (Apple
    // has no such endpoint; the trust token lapses on its own after 30 days,
    // and the device can be removed under the Apple ID's devices.)
    const QString refresh = QJsonDocument::fromJson(token).object().value(QLatin1String("refresh_token")).toString();
    if (m_provider == GoogleDrive && !refresh.isEmpty()) {
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
    // No point keeping a mount (or retrying every 10 s) with dead credentials;
    // edits not yet uploaded stay in the cache and go up after signing in.
    systemctl({QStringLiteral("stop"), unit()});
    setState(SignedOut, signedOutText());
}

void FinderCloud::followLog()
{
    if (m_journal || !hasAccount()) {
        return;
    }
    // A running mount doesn't exit when its credentials are revoked or
    // expire; it only logs failed requests.
    auto *journal = new QProcess(this);
    m_journal = journal;
    connect(journal, &QProcess::readyReadStandardOutput, this, [this, journal]() {
        while (journal->canReadLine()) {
            const QString line = QString::fromUtf8(journal->readLine());
            if (m_state != SignedOut && m_state != SigningIn && matchesSignedOut(line)) {
                signedOut();
            }
        }
    });
    connect(journal, &QProcess::finished, journal, &QObject::deleteLater);
    journal->start(QStringLiteral("journalctl"),
                   {QStringLiteral("--user"), QStringLiteral("-u"), unit(), QStringLiteral("-f"), QStringLiteral("-n"), QStringLiteral("0"), QStringLiteral("-o"),
                    QStringLiteral("cat")});
}

void FinderCloud::poll()
{
    // Signed out stays so until the user signs in again (the service is
    // stopped, and may still be unmounting).
    if (m_state == SigningIn || (m_state == SignedOut && hasAccount())) {
        return;
    }
    if (!hasAccount()) {
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
    if ((m_state == SignedOut || m_state == Failed) && ++m_checkSkips % 5 != 0) {
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
            if (matchesSignedOut(log)) {
                signedOut();
                return;
            }
            QString reason;
            for (const QString &line : log.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (line.contains(QLatin1String("ERROR")) || line.contains(QLatin1String("CRITICAL")) || line.startsWith(QLatin1String("Fatal"))) {
                    reason = line.section(QStringLiteral(": "), 1).trimmed();
                }
            }
            setState(Failed, reason.isEmpty() ? i18nc("@info Finder cloud drive, %1 is e.g. iCloud Drive", "%1 couldn't start.", name()) : reason);
        });
        journal->start(QStringLiteral("journalctl"),
                       {QStringLiteral("--user"), QStringLiteral("-u"), unit(), QStringLiteral("-n"), QStringLiteral("30"), QStringLiteral("-o"), QStringLiteral("cat"),
                        QStringLiteral("--no-pager")});
    });
    process->start(QStringLiteral("systemctl"),
                   {QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("-p"), QStringLiteral("ActiveState"), QStringLiteral("-p"),
                    QStringLiteral("SubState"), QStringLiteral("-p"), QStringLiteral("Result"), unit()});
}

namespace
{
// One call to rclone's remote control API: HTTP/1.0 over the unix socket, so
// the reply comes back whole, without chunking, and the socket then closes.
void rcCall(QObject *context, const QString &socketPath, const QString &method, std::function<void(const QJsonObject &, const QByteArray &)> done,
            const QByteArray &params = "{}")
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
    socket->connectToServer(socketPath);
}
}

void FinderCloud::requestStats()
{
    if (m_statsPending) {
        return;
    }
    m_statsPending = true;
    // Once a minute, ask the provider itself (for the quota): a mount whose
    // credentials were revoked only fails when something touches the drive.
    if (++m_statsPolls % 30 == 1) {
        rcCall(
            this, socketPath(), QStringLiteral("operations/about"),
            [this](const QJsonObject &, const QByteArray &raw) {
                if (m_state == Ready && matchesSignedOut(QString::fromUtf8(raw))) {
                    signedOut();
                }
            },
            "{\"fs\":\"" + remote().toLatin1() + ":\"}");
    }
    rcCall(this, socketPath(), QStringLiteral("vfs/stats"), [this](const QJsonObject &vfs, const QByteArray &) {
        rcCall(this, socketPath(), QStringLiteral("core/stats"), [this, vfs](const QJsonObject &core, const QByteArray &) {
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
