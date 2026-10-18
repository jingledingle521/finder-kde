/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "filechooserportal.h"
#include "finderfiledialog.h"

#include <KLocalizedString>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>

QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilterRule &rule)
{
    arg.beginStructure();
    arg << rule.type << rule.pattern;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilterRule &rule)
{
    arg.beginStructure();
    arg >> rule.type >> rule.pattern;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilter &filter)
{
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilter &filter)
{
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}

namespace
{
// Portal response codes.
constexpr uint Success = 0;
constexpr uint Cancelled = 1;

/** The Request object xdg-desktop-portal uses to cancel a pending dialog. */
class PortalRequest : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    using QObject::QObject;

public Q_SLOTS:
    void Close()
    {
        Q_EMIT closeRequested();
    }

Q_SIGNALS:
    void closeRequested();
};

/** Byte-array paths ("ay", NUL-terminated) as the portal sends them. */
QString bytesPath(const QVariant &value)
{
    // Arrives wrapped in a QDBusArgument when read from an a{sv}.
    QByteArray bytes = value.userType() == qMetaTypeId<QDBusArgument>() ? qdbus_cast<QByteArray>(value.value<QDBusArgument>()) : value.toByteArray();
    while (bytes.endsWith('\0')) {
        bytes.chop(1);
    }
    return QFile::decodeName(bytes);
}
}

FileChooserPortal::FileChooserPortal(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<PortalFilterRule>();
    qDBusRegisterMetaType<PortalFilterRules>();
    qDBusRegisterMetaType<PortalFilter>();
    qDBusRegisterMetaType<PortalFilterList>();
}

int FileChooserPortal::openRequests() const
{
    return m_open;
}

uint FileChooserPortal::OpenFile(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title, const QVariantMap &options, QVariantMap &)
{
    request(Kind::Open, handle, title, options);
    return 0; // the real answer is sent when the dialog closes
}

uint FileChooserPortal::SaveFile(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title, const QVariantMap &options, QVariantMap &)
{
    request(Kind::Save, handle, title, options);
    return 0;
}

uint FileChooserPortal::SaveFiles(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title, const QVariantMap &options, QVariantMap &)
{
    request(Kind::SaveFiles, handle, title, options);
    return 0;
}

void FileChooserPortal::request(Kind kind, const QDBusObjectPath &handle, const QString &title, const QVariantMap &options)
{
    FinderFileDialog::Options o;
    o.mode = kind == Kind::Open ? FinderFileDialog::Open : FinderFileDialog::Save;
    o.title = title;
    o.acceptLabel = options.value(QStringLiteral("accept_label")).toString();
    o.multiple = kind == Kind::Open && options.value(QStringLiteral("multiple")).toBool();
    // SaveFiles saves several files into one folder the user picks.
    o.directory = options.value(QStringLiteral("directory")).toBool() || kind == Kind::SaveFiles;
    if (kind == Kind::SaveFiles) {
        o.mode = FinderFileDialog::Open;
        if (o.acceptLabel.isEmpty()) {
            o.acceptLabel = i18nc("@action:button Finder file chooser", "Save Here");
        }
    }
    if (options.contains(QStringLiteral("filters"))) {
        o.filters = qdbus_cast<PortalFilterList>(options.value(QStringLiteral("filters")).value<QDBusArgument>());
    }
    if (options.contains(QStringLiteral("current_filter"))) {
        const PortalFilter current = qdbus_cast<PortalFilter>(options.value(QStringLiteral("current_filter")).value<QDBusArgument>());
        for (int i = 0; i < o.filters.size(); ++i) {
            if (o.filters.at(i).name == current.name) {
                o.currentFilter = i;
            }
        }
        if (o.filters.isEmpty() && !current.rules.isEmpty()) {
            o.filters = {current};
            o.currentFilter = 0;
        }
    }
    o.currentName = options.value(QStringLiteral("current_name")).toString();
    QString folder = bytesPath(options.value(QStringLiteral("current_folder")));

    const QString file = bytesPath(options.value(QStringLiteral("current_file")));
    if (!file.isEmpty()) {
        folder = QFileInfo(file).absolutePath();
        if (o.currentName.isEmpty()) {
            o.currentName = QFileInfo(file).fileName();
        }
    }
    o.folder = QUrl::fromLocalFile(folder.isEmpty() || !QDir(folder).exists() ? QDir::homePath() : folder);
    QStringList saveNames; // SaveFiles: the names to save into the chosen folder
    if (kind == Kind::SaveFiles) {
        for (const QByteArray &name : qdbus_cast<QList<QByteArray>>(options.value(QStringLiteral("files")).value<QDBusArgument>())) {
            saveNames.append(bytesPath(name));
        }
    }

    // The answer goes out when the dialog closes.
    setDelayedReply(true);
    const QDBusMessage message = this->message();

    auto *dialog = new FinderFileDialog(o);
    auto *request = new PortalRequest(dialog);
    QDBusConnection::sessionBus().registerObject(handle.path(), request, QDBusConnection::ExportAllSlots);
    connect(request, &PortalRequest::closeRequested, dialog, &FinderFileDialog::cancel);
    ++m_open;

    connect(dialog, &FinderFileDialog::finished, this, [this, dialog, message, handle, kind, saveNames](bool accepted, const QList<QUrl> &urls, int filter) {
        QDBusConnection::sessionBus().unregisterObject(handle.path());
        QVariantMap results;
        if (accepted) {
            QStringList uris;
            if (kind == Kind::SaveFiles) {
                const QUrl folder = urls.value(0);
                for (const QString &name : saveNames) {
                    uris.append(QUrl::fromLocalFile(QDir(folder.toLocalFile()).filePath(name)).toString());
                }
            } else {
                for (const QUrl &url : urls) {
                    uris.append(url.toString());
                }
            }
            results.insert(QStringLiteral("uris"), uris);
            if (filter >= 0 && filter < dialog->options().filters.size()) {
                results.insert(QStringLiteral("current_filter"), QVariant::fromValue(dialog->options().filters.at(filter)));
            }
        }
        QDBusConnection::sessionBus().send(message.createReply({QVariant::fromValue(accepted ? Success : Cancelled), QVariant::fromValue(results)}));
        dialog->deleteLater();
        if (--m_open == 0) {
            Q_EMIT idle();
        }
    });
    dialog->present();
}

#include "filechooserportal.moc"
#include "moc_filechooserportal.cpp"
