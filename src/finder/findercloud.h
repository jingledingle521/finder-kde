/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FINDERCLOUD_H
#define FINDERCLOUD_H

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>

class QProcess;

/**
 * @brief Finder's Google Drive, run by rclone (built from 3rdparty/rclone).
 *
 * One per process. Signing in runs `rclone authorize drive`, which opens
 * Google's consent page and hands back a token; Finder keeps it in its own
 * rclone config (~/.config/finder/rclone.conf, owner-only). The drive is then
 * mounted at ~/Google Drive by a systemd user service that Finder writes and
 * enables, so it stays mounted while Finder is closed and comes back at login.
 * Finder follows the mount through rclone's remote control API on a unix
 * socket: pending uploads and downloads, and files that failed to upload.
 */
class FinderCloud : public QObject
{
    Q_OBJECT

public:
    enum State {
        NotConnected, // no account
        SigningIn, // waiting on the browser
        Starting, // signed in, service not mounted yet
        Ready, // mounted
        SignedOut, // Google refused the token: sign in again
        Failed, // the service failed for another reason
    };
    Q_ENUM(State)

    static FinderCloud *instance();

    /** ~/Google Drive, where the drive is mounted. */
    static QString mountPath();
    static QUrl webUrl();

    /** The rclone binary Finder uses, or empty if there is none. */
    QString rclone() const;

    /** Signed in (even if signed out by Google since, or not mounted yet). */
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
     * Opens Google's sign-in page in the browser and waits for it. On success
     * the drive is set up and started; @p done gets the error otherwise
     * (empty when cancelled).
     */
    void signIn(std::function<void(bool ok, const QString &error)> done);
    void cancelSignIn();
    /** The sign-in page, while signing in, to open it again. */
    QUrl signInUrl() const;

    /** Starts the mount if it isn't running (it normally starts at login). */
    void start();
    /** Stops the mount and forgets the account: token, service and local cache. */
    void disconnect();

Q_SIGNALS:
    void stateChanged(FinderCloud::State state);
    void activityChanged();
    void signInUrlChanged(const QUrl &url);

private:
    FinderCloud();
    void setState(State state, const QString &error = {});
    void writeConfig(const QByteArray &token);
    bool writeUnit();
    void systemctl(const QStringList &args, std::function<void(bool ok)> done = {});
    void poll();
    void checkService();
    void requestStats();
    QString authTemplate();

    State m_state = NotConnected;
    QString m_error;
    QTimer m_pollTimer;
    bool m_checking = false; // a systemctl/journal check is running
    bool m_statsPending = false;

    QPointer<QProcess> m_authorize;
    QUrl m_signInUrl;
    std::function<void(bool, const QString &)> m_signInDone;

    int m_uploads = 0;
    int m_downloads = 0;
    int m_uploadErrors = 0;
};

#endif
