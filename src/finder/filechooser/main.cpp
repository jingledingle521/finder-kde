/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// finder-filechooser: Finder's Open/Save windows for xdg-desktop-portal.
// Started by D-Bus on the first request; quits after a while with nothing open.

#include "filechooserportal.h"
#include "../finderhangwatch.h"

#include <KAboutData>
#include <KLocalizedString>

#include <QApplication>
#include <QIcon>
#include <QDBusConnection>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Log where the dialog is stuck when it stops responding (shares Finder's hangs.log).
    FinderHangWatch::start();
    FinderHangWatch::note(QStringLiteral("Open/Save dialog"));
    app.setQuitOnLastWindowClosed(false);
    KLocalizedString::setApplicationDomain("dolphin");

    // Same component as Finder: shares its settings and per-folder view setup.
    KAboutData about(QStringLiteral("finder"), i18nc("@title", "Finder"), QStringLiteral("1.0"));
    about.setDesktopFileName(QStringLiteral("org.brhett.finder"));
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("system-file-manager")));

    auto *portal = new FileChooserPortal(&app);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), portal, QDBusConnection::ExportAllSlots)
        || !bus.registerService(QStringLiteral("org.freedesktop.impl.portal.desktop.finder"))) {
        qWarning("finder-filechooser: could not register on the session bus (already running?)");
        return 1;
    }

    // Quit when idle; D-Bus starts us again on the next request.
    QTimer idle;
    idle.setSingleShot(true);
    idle.setInterval(5 * 60 * 1000);
    QObject::connect(&idle, &QTimer::timeout, &app, [portal]() {
        if (portal->openRequests() == 0) {
            QCoreApplication::quit();
        }
    });
    QObject::connect(portal, &FileChooserPortal::idle, &idle, qOverload<>(&QTimer::start));
    idle.start();

    return app.exec();
}
