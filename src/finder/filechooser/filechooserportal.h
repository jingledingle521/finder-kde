/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FILECHOOSERPORTAL_H
#define FILECHOOSERPORTAL_H

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariantMap>

/** One rule of a portal file filter: type 0 is a glob ("*.png"), 1 a MIME type. */
struct PortalFilterRule {
    uint type = 0;
    QString pattern;
};
using PortalFilterRules = QList<PortalFilterRule>;

/** A named portal file filter ("Images": *.png, image/jpeg…), D-Bus type (sa(us)). */
struct PortalFilter {
    QString name;
    PortalFilterRules rules;
};
using PortalFilterList = QList<PortalFilter>;

Q_DECLARE_METATYPE(PortalFilterRule)
Q_DECLARE_METATYPE(PortalFilter)

/**
 * @brief Finder's file chooser for xdg-desktop-portal
 * (org.freedesktop.impl.portal.FileChooser).
 *
 * Apps that ask the desktop for an Open or Save dialog (browsers, Flatpak and
 * Snap apps, Electron apps…) get a Finder-style window. Each request is
 * answered when its window closes; xdg-desktop-portal can cancel it through
 * the org.freedesktop.impl.portal.Request object at the request's handle.
 */
class FileChooserPortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")

public:
    explicit FileChooserPortal(QObject *parent = nullptr);

    /** Requests (dialogs) still open; the service quits when idle for a while. */
    int openRequests() const;

public Q_SLOTS:
    uint OpenFile(const QDBusObjectPath &handle,
                  const QString &app_id,
                  const QString &parent_window,
                  const QString &title,
                  const QVariantMap &options,
                  QVariantMap &results);
    uint SaveFile(const QDBusObjectPath &handle,
                  const QString &app_id,
                  const QString &parent_window,
                  const QString &title,
                  const QVariantMap &options,
                  QVariantMap &results);
    uint SaveFiles(const QDBusObjectPath &handle,
                   const QString &app_id,
                   const QString &parent_window,
                   const QString &title,
                   const QVariantMap &options,
                   QVariantMap &results);

Q_SIGNALS:
    void idle();

private:
    enum class Kind {
        Open,
        Save,
        SaveFiles,
    };
    void request(Kind kind, const QDBusObjectPath &handle, const QString &title, const QVariantMap &options);

    int m_open = 0;
};

#endif
