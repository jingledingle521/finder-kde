/*
 * SPDX-FileCopyrightText: 2026 Brhett
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "findercloudsheet.h"
#include "findercloud.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
QLabel *driveGlyph(QWidget *parent, int size)
{
    auto *label = new QLabel(parent);
    QIcon icon = QIcon::fromTheme(QStringLiteral("folder-google-drive"), QIcon::fromTheme(QStringLiteral("folder-cloud")));
    label->setPixmap(icon.pixmap(QSize(size, size), parent->devicePixelRatioF()));
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.15);
    label->setFont(font);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

QLabel *bodyText(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    return label;
}
}

FinderCloudSheet::FinderCloudSheet(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "Google Drive"));
    setModal(true);
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 18);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);

    auto *cloud = FinderCloud::instance();
    const bool again = cloud->state() == FinderCloud::SignedOut;

    // Page 1: what connecting does.
    auto *intro = new QWidget(m_pages);
    auto *i = new QVBoxLayout(intro);
    i->setSpacing(10);
    i->addWidget(driveGlyph(intro, 64));
    i->addWidget(heading(again ? i18nc("@title Finder Google Drive", "Sign in to Google Drive again")
                               : i18nc("@title Finder Google Drive", "Connect Google Drive"),
                         intro));
    i->addWidget(bodyText(again ? cloud->errorText() + QLatin1Char(' ')
                                      + i18nc("@info Finder Google Drive", "Changes you made meanwhile are kept on this computer and upload once you're back.")
                                : i18nc("@info Finder Google Drive",
                                        "Your Drive shows up in the sidebar as a folder that works with every app. "
                                        "Files you open are kept on this computer, and changes sync back to Google. "
                                        "Google Docs, Sheets and Slides open as Word, Excel and PowerPoint files."),
                          intro));
    m_error = new QLabel(intro);
    m_error->setAlignment(Qt::AlignCenter);
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color: #ff9f0a;"));
    m_error->hide();
    i->addWidget(m_error);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(i18nc("@action:button", "Cancel"), intro);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_signInButton = new QPushButton(i18nc("@action:button Finder Google Drive", "Sign In with Google…"), intro);
    m_signInButton->setDefault(true);
    connect(m_signInButton, &QPushButton::clicked, this, &FinderCloudSheet::signIn);
    buttons->addWidget(m_signInButton);
    i->addLayout(buttons);
    m_pages->addWidget(intro);

    // Page 2: waiting on the browser.
    auto *waiting = new QWidget(m_pages);
    auto *w = new QVBoxLayout(waiting);
    w->setSpacing(10);
    w->addWidget(driveGlyph(waiting, 64));
    w->addWidget(heading(i18nc("@title Finder Google Drive", "Continue in your browser"), waiting));
    w->addWidget(bodyText(i18nc("@info Finder Google Drive",
                                "Choose your Google account and allow access to Drive. "
                                "When the page says Google Drive is connected, come back here."),
                          waiting));
    auto *row = new QHBoxLayout;
    auto *reopen = new QPushButton(i18nc("@action:button Finder Google Drive", "Open the Page Again"), waiting);
    reopen->setEnabled(false);
    connect(reopen, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(FinderCloud::instance()->signInUrl());
    });
    connect(cloud, &FinderCloud::signInUrlChanged, reopen, [reopen](const QUrl &url) {
        reopen->setEnabled(url.isValid());
    });
    row->addWidget(reopen);
    row->addStretch(1);
    auto *stop = new QPushButton(i18nc("@action:button", "Cancel"), waiting);
    connect(stop, &QPushButton::clicked, this, &QDialog::reject);
    row->addWidget(stop);
    w->addLayout(row);
    m_pages->addWidget(waiting);

    // Page 3: signed in, waiting for the drive to mount.
    auto *settingUp = new QWidget(m_pages);
    auto *s = new QVBoxLayout(settingUp);
    s->setSpacing(10);
    s->addWidget(driveGlyph(settingUp, 64));
    s->addWidget(heading(i18nc("@title Finder Google Drive", "Setting up Google Drive…"), settingUp));
    m_settingUp = bodyText(i18nc("@info Finder Google Drive", "Connecting to your Drive. This takes a few seconds."), settingUp);
    s->addWidget(m_settingUp);
    m_pages->addWidget(settingUp);

    connect(cloud, &FinderCloud::stateChanged, this, [this](FinderCloud::State state) {
        if (!m_waitingForMount) {
            return;
        }
        if (state == FinderCloud::Ready) {
            accept();
        } else if (state == FinderCloud::Failed || state == FinderCloud::SignedOut) {
            m_waitingForMount = false;
            showError(FinderCloud::instance()->errorText());
        }
    });
}

FinderCloudSheet::~FinderCloudSheet()
{
    // Closed while the browser was still open.
    if (m_pages->currentIndex() == 1) {
        FinderCloud::instance()->cancelSignIn();
    }
}

void FinderCloudSheet::signIn()
{
    m_error->hide();
    m_pages->setCurrentIndex(1);
    FinderCloud::instance()->signIn([sheet = QPointer<FinderCloudSheet>(this)](bool ok, const QString &error) {
        if (!sheet) {
            return;
        }
        if (!ok) {
            if (error.isEmpty()) {
                sheet->reject(); // cancelled
            } else {
                sheet->showError(error);
            }
            return;
        }
        sheet->m_pages->setCurrentIndex(2);
        if (FinderCloud::instance()->state() == FinderCloud::Ready) {
            sheet->accept();
            return;
        }
        sheet->m_waitingForMount = true;
        QTimer::singleShot(45000, sheet, [sheet]() {
            if (sheet && sheet->m_waitingForMount) {
                sheet->m_settingUp->setText(i18nc("@info Finder Google Drive",
                                                  "This is taking longer than usual. You can close this; "
                                                  "Google Drive appears in the sidebar once it's ready."));
            }
        });
    });
}

void FinderCloudSheet::showError(const QString &error)
{
    m_pages->setCurrentIndex(0);
    m_signInButton->setText(i18nc("@action:button Finder Google Drive", "Try Again"));
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
}

#include "moc_findercloudsheet.cpp"
