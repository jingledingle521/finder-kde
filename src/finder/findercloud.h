/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCLOUD_H
#define FINDERCLOUD_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>

class QProcess;

/**
 * @brief A cloud drive in Finder (Google Drive or iCloud Drive), run by
 * rclone (built from 3rdparty/rclone).
 *
 * One per provider and process. Each signs in its own way (see signIn() and
 * signInWithPassword()) and keeps its credentials in its own rclone config
 * under ~/.config/finder, owner-only. The drive is then mounted in the home
 * folder by a systemd user service that Finder writes and enables, so it
 * stays mounted while Finder is closed and comes back at login. Finder
 * follows the mount through rclone's remote control API on a unix socket:
 * pending uploads and downloads, and files that failed to upload.
 */
class FinderCloud : public QObject
{
    Q_OBJECT

public:
    enum Provider {
        GoogleDrive,
        ICloudDrive,
    };
    Q_ENUM(Provider)

    enum State {
        NotConnected, // no account
        SigningIn, // waiting on the browser, or on the user's answers
        Starting, // signed in, service not mounted yet
        Ready, // mounted
        SignedOut, // the provider refused the credentials: sign in again
        Failed, // the service failed for another reason
    };
    Q_ENUM(State)

    static FinderCloud *instance(Provider provider);
    static QList<FinderCloud *> all();

    Provider provider() const;
    /** "Google Drive", "iCloud Drive". */
    QString name() const;
    /** The folder it's mounted at: ~/Google Drive, ~/iCloud Drive. */
    QString mountPath() const;
    QUrl webUrl() const;
    /** Full-color and sidebar (symbolic) icon names. */
    QString iconName() const;
    QString symbolicIconName() const;

    /** The rclone binary Finder uses, or empty if there is none. */
    static QString rclone();

    /** The account signed in to (iCloud's Apple ID), if Finder knows it. */
    QString accountName() const;
    /** Signed in (even if signed out by the provider since, or not mounted yet). */
    bool hasAccount() const;
    State state() const;
    /** Why the state is SignedOut or Failed, in words. */
    QString errorText() const;

    /** Files waiting to upload or uploading, and files being downloaded. */
    int uploads() const;
    int downloads() const;
    /** Files rclone gave up uploading (kept in the cache and retried). */
    int uploadErrors() const;
    bool isBusy() const;
    /** "Up to date", "Uploading 3 files…", for the sidebar tooltip and menu. */
    QString activityText() const;

    /**
     * Google Drive: opens Google's sign-in page in the browser and waits for
     * it. On success the drive is set up and started; @p done gets the error
     * otherwise (empty when cancelled).
     */
    void signIn(std::function<void(bool ok, const QString &error)> done);
    /** The sign-in page, while signing in, to open it again. */
    QUrl signInUrl() const;

    /** A question iCloud asks while signing in, or how signing in ended. */
    struct Step {
        enum Kind {
            Done, // signed in; the drive is starting
            Code, // a verification code: type it, or "sms" for a text instead
            Choice, // pick where to send the text (choices: value, label)
            Error, // signing in stopped; message says why
        };
        Kind kind = Error;
        QString prompt;
        QList<std::pair<QString, QString>> choices;
        QString message;
    };
    /**
     * iCloud Drive: signs in with the Apple ID and password the way
     * icloud.com does (the password itself never leaves this computer), then
     * asks for the code Apple sends to trusted devices. @p next gets each
     * question in turn; answer() replies to it.
     */
    void signInWithPassword(const QString &appleId, const QString &password, std::function<void(const Step &)> next);
    void answer(const QString &result, std::function<void(const Step &)> next);

    void cancelSignIn();

    /** Starts the mount if it isn't running (it normally starts at login). */
    void start();
    /** Stops the mount and forgets the account: credentials, service and local cache. */
    void disconnect();

Q_SIGNALS:
    void stateChanged(FinderCloud::State state);
    void activityChanged();
    void signInUrlChanged(const QUrl &url);

private:
    explicit FinderCloud(Provider provider);
    void setState(State state, const QString &error = {});
    QString remote() const;
    QString unit() const;
    QString configPath() const;
    QString unitPath() const;
    QString cachePath() const;
    QString socketPath() const;
    /** A value from this drive's section of its rclone config. */
    QByteArray configValue(const QByteArray &key) const;
    bool matchesSignedOut(const QString &log) const;
    QString signedOutText() const;
    void writeGoogleConfig(const QByteArray &token);
    bool writeUnit();
    void systemctl(const QStringList &args, std::function<void(bool ok)> done = {});
    /** Signed in: write the service, then (re)start it. */
    void finishSignIn();
    /** Runs one step of rclone's non-interactive config and reports it. */
    void runConfigStep(const QStringList &args, std::function<void(const Step &)> next);
    void abandonSignIn();
    void poll();
    void checkService();
    void followLog();
    void signedOut();
    void requestStats();
    QString authTemplate();

    Provider m_provider;
    State m_state = NotConnected;
    QString m_error;
    QTimer m_pollTimer;
    bool m_checking = false; // a systemctl/journal check is running
    int m_checkSkips = 0; // checks skipped while signed out or failed
    bool m_statsPending = false;
    int m_statsPolls = 0;

    QPointer<QProcess> m_journal; // following the service's log for auth errors
    QPointer<QProcess> m_signInProcess;
    QUrl m_signInUrl;
    std::function<void(bool, const QString &)> m_signInDone;
    State m_stateBeforeSignIn = NotConnected;
    QString m_errorBeforeSignIn;
    QString m_configState; // where rclone's iCloud sign-in is waiting

    int m_uploads = 0;
    int m_downloads = 0;
    int m_uploadErrors = 0;
};

#endif
