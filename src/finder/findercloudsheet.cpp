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
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
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

QLabel *iconGlyph(const QString &name, QWidget *parent, int size)
{
    auto *label = new QLabel(parent);
    label->setPixmap(QIcon::fromTheme(name, QIcon::fromTheme(QStringLiteral("folder-cloud"))).pixmap(QSize(size, size), parent->devicePixelRatioF()));
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QLabel *noteText(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 130);"));
    return label;
}

QLabel *errorLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: #ff9f0a;"));
    label->hide();
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

QDialog *FinderCloudSheet::create(FinderCloud *cloud, QWidget *parent)
{
    if (cloud->provider() == FinderCloud::ICloudDrive) {
        return new FinderICloudSheet(cloud, parent);
    }
    return new FinderCloudSheet(cloud, parent);
}

FinderCloudSheet::FinderCloudSheet(FinderCloud *cloud, QWidget *parent)
    : QDialog(parent)
    , m_cloud(cloud)
{
    setWindowTitle(i18nc("@title:window", "Google Drive"));
    setModal(true);
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 18);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);

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
    connect(reopen, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(m_cloud->signInUrl());
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
            showError(m_cloud->errorText());
        }
    });
}

FinderCloudSheet::~FinderCloudSheet()
{
    // Closed while the browser was still open.
    if (m_pages->currentIndex() == 1) {
        m_cloud->cancelSignIn();
    }
}

void FinderCloudSheet::signIn()
{
    m_error->hide();
    m_pages->setCurrentIndex(1);
    m_cloud->signIn([sheet = QPointer<FinderCloudSheet>(this)](bool ok, const QString &error) {
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
        if (sheet->m_cloud->state() == FinderCloud::Ready) {
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

FinderICloudSheet::FinderICloudSheet(FinderCloud *cloud, QWidget *parent)
    : QDialog(parent)
    , m_cloud(cloud)
{
    setWindowTitle(cloud->name());
    setModal(true);
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 18);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);
    const bool again = cloud->state() == FinderCloud::SignedOut;

    // Page 1: Apple ID and password.
    auto *account = new QWidget(m_pages);
    auto *a = new QVBoxLayout(account);
    a->setSpacing(10);
    a->addWidget(iconGlyph(cloud->iconName(), account, 64));
    a->addWidget(heading(again ? i18nc("@title Finder iCloud", "Sign in to iCloud again") : i18nc("@title Finder iCloud", "Sign in to iCloud Drive"), account));
    a->addWidget(bodyText(again ? cloud->errorText() + QLatin1Char(' ')
                                      + i18nc("@info Finder iCloud", "Changes you made meanwhile are kept on this computer and upload once you're back.")
                                : i18nc("@info Finder iCloud",
                                        "Your iCloud Drive shows up in the sidebar as a folder that works with every app. "
                                        "Files you open are kept on this computer, and changes sync back to iCloud."),
                          account));
    m_appleId = new QLineEdit(cloud->accountName(), account);
    m_appleId->setPlaceholderText(i18nc("@info:placeholder", "Apple Account (email or phone number)"));
    m_password = new QLineEdit(account);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(i18nc("@info:placeholder", "Password"));
    a->addWidget(m_appleId);
    a->addWidget(m_password);
    a->addWidget(noteText(i18nc("@info Finder iCloud",
                                "Finder signs in the way icloud.com does: your password stays on this computer. "
                                "\"Access iCloud Data on the Web\" must be on (Settings › your name › iCloud on an iPhone). "
                                "With Advanced Data Protection, you'll also approve this computer on a trusted device."),
                          account));
    m_error = errorLabel(account);
    a->addWidget(m_error);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(i18nc("@action:button", "Cancel"), account);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_signInButton = new QPushButton(i18nc("@action:button Finder iCloud", "Sign In"), account);
    m_signInButton->setDefault(true);
    connect(m_signInButton, &QPushButton::clicked, this, &FinderICloudSheet::signIn);
    buttons->addWidget(m_signInButton);
    a->addLayout(buttons);
    auto updateSignIn = [this]() {
        m_signInButton->setEnabled(!m_signingIn && !m_appleId->text().trimmed().isEmpty() && !m_password->text().isEmpty());
    };
    connect(m_appleId, &QLineEdit::textChanged, this, updateSignIn);
    connect(m_password, &QLineEdit::textChanged, this, updateSignIn);
    connect(m_password, &QLineEdit::returnPressed, this, [this]() {
        if (m_signInButton->isEnabled()) {
            signIn();
        }
    });
    updateSignIn();
    m_pages->addWidget(account);

    // Page 2: the verification code.
    auto *code = new QWidget(m_pages);
    auto *c = new QVBoxLayout(code);
    c->setSpacing(10);
    c->addWidget(iconGlyph(cloud->iconName(), code, 64));
    c->addWidget(heading(i18nc("@title Finder iCloud", "Two-Factor Authentication"), code));
    m_codePrompt = bodyText(QString(), code);
    c->addWidget(m_codePrompt);
    m_code = new QLineEdit(code);
    m_code->setAlignment(Qt::AlignCenter);
    m_code->setMaxLength(8);
    m_code->setPlaceholderText(QStringLiteral("• • • • • •"));
    QFont codeFont = m_code->font();
    codeFont.setPointSizeF(codeFont.pointSizeF() * 1.6);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 6);
    m_code->setFont(codeFont);
    m_code->setMaximumWidth(220);
    c->addWidget(m_code, 0, Qt::AlignCenter);
    m_codeError = errorLabel(code);
    c->addWidget(m_codeError);
    auto *codeButtons = new QHBoxLayout;
    m_textButton = new QPushButton(i18nc("@action:button Finder iCloud", "Text Me a Code Instead"), code);
    connect(m_textButton, &QPushButton::clicked, this, [this]() {
        sendCode(QStringLiteral("sms"));
    });
    codeButtons->addWidget(m_textButton);
    codeButtons->addStretch(1);
    auto *codeCancel = new QPushButton(i18nc("@action:button", "Cancel"), code);
    connect(codeCancel, &QPushButton::clicked, this, &QDialog::reject);
    codeButtons->addWidget(codeCancel);
    m_verifyButton = new QPushButton(i18nc("@action:button Finder iCloud", "Verify"), code);
    m_verifyButton->setDefault(true);
    m_verifyButton->setEnabled(false);
    connect(m_verifyButton, &QPushButton::clicked, this, [this]() {
        sendCode(m_code->text().trimmed());
    });
    codeButtons->addWidget(m_verifyButton);
    c->addLayout(codeButtons);
    connect(m_code, &QLineEdit::textChanged, this, [this](const QString &text) {
        const QString digits = text.trimmed();
        m_verifyButton->setEnabled(!digits.isEmpty());
        // Apple's codes are six digits: go on as soon as the last one is in.
        static const QRegularExpression sixDigits(QStringLiteral("^\\d{6}$"));
        if (sixDigits.match(digits).hasMatch() && m_code->isEnabled()) {
            sendCode(digits);
        }
    });
    connect(m_code, &QLineEdit::returnPressed, m_verifyButton, &QPushButton::click);
    m_pages->addWidget(code);

    // Page 3: which trusted number to text.
    auto *phones = new QWidget(m_pages);
    auto *ph = new QVBoxLayout(phones);
    ph->setSpacing(10);
    ph->addWidget(iconGlyph(cloud->iconName(), phones, 64));
    ph->addWidget(heading(i18nc("@title Finder iCloud", "Where should Apple send the code?"), phones));
    m_phones = new QVBoxLayout;
    m_phones->setSpacing(6);
    ph->addLayout(m_phones);
    auto *phoneButtons = new QHBoxLayout;
    phoneButtons->addStretch(1);
    auto *phoneCancel = new QPushButton(i18nc("@action:button", "Cancel"), phones);
    connect(phoneCancel, &QPushButton::clicked, this, &QDialog::reject);
    phoneButtons->addWidget(phoneCancel);
    ph->addLayout(phoneButtons);
    m_pages->addWidget(phones);

    // Page 4: signed in, waiting for the drive to mount.
    auto *settingUp = new QWidget(m_pages);
    auto *s = new QVBoxLayout(settingUp);
    s->setSpacing(10);
    s->addWidget(iconGlyph(cloud->iconName(), settingUp, 64));
    s->addWidget(heading(i18nc("@title Finder iCloud", "Setting up iCloud Drive…"), settingUp));
    m_settingUp = bodyText(i18nc("@info Finder iCloud", "Connecting to your iCloud Drive. This takes a few seconds."), settingUp);
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
            showError(m_cloud->errorText());
        }
    });
    (m_appleId->text().isEmpty() ? m_appleId : m_password)->setFocus();
}

FinderICloudSheet::~FinderICloudSheet()
{
    // Closed halfway through: forget the half-made sign-in.
    if (m_signingIn) {
        m_cloud->cancelSignIn();
    }
}

void FinderICloudSheet::setBusy(bool busy)
{
    for (QWidget *widget : {static_cast<QWidget *>(m_appleId), static_cast<QWidget *>(m_password), static_cast<QWidget *>(m_code),
                            static_cast<QWidget *>(m_verifyButton), static_cast<QWidget *>(m_textButton)}) {
        widget->setEnabled(!busy);
    }
    m_signInButton->setEnabled(!busy);
    m_signInButton->setText(busy ? i18nc("@action:button Finder iCloud", "Signing In…") : i18nc("@action:button Finder iCloud", "Sign In"));
    m_verifyButton->setText(busy ? i18nc("@action:button Finder iCloud", "Verifying…") : i18nc("@action:button Finder iCloud", "Verify"));
    if (!busy) {
        m_verifyButton->setEnabled(!m_code->text().trimmed().isEmpty());
    }
}

void FinderICloudSheet::signIn()
{
    m_error->hide();
    m_signingIn = true;
    setBusy(true);
    m_cloud->signInWithPassword(m_appleId->text().trimmed(), m_password->text(), [sheet = QPointer<FinderICloudSheet>(this)](const FinderCloud::Step &step) {
        if (sheet) {
            sheet->showStep(step);
        }
    });
}

void FinderICloudSheet::sendCode(const QString &code)
{
    m_codeError->hide();
    setBusy(true);
    m_cloud->answer(code, [sheet = QPointer<FinderICloudSheet>(this)](const FinderCloud::Step &step) {
        if (sheet) {
            sheet->showStep(step);
        }
    });
}

void FinderICloudSheet::showStep(const FinderCloud::Step &step)
{
    setBusy(false);
    switch (step.kind) {
    case FinderCloud::Step::Done:
        m_signingIn = false;
        m_password->clear();
        m_pages->setCurrentIndex(3);
        if (m_cloud->state() == FinderCloud::Ready) {
            accept();
            return;
        }
        m_waitingForMount = true;
        QTimer::singleShot(45000, this, [sheet = QPointer<FinderICloudSheet>(this)]() {
            if (sheet && sheet->m_waitingForMount) {
                sheet->m_settingUp->setText(i18nc("@info Finder iCloud",
                                                  "This is taking longer than usual. With Advanced Data Protection, approve this computer on "
                                                  "your iPhone, iPad or Mac. You can close this; iCloud Drive appears in the sidebar once it's ready."));
            }
        });
        return;
    case FinderCloud::Step::Code: {
        m_pages->setCurrentIndex(1);
        // rclone's prompt names the number for a text; for trusted devices
        // it only explains the command line, so say it the way Apple does.
        const bool texted = step.prompt.contains(QLatin1String("sent to"));
        m_codePrompt->setText(texted ? step.prompt + QLatin1Char('.')
                                     : i18nc("@info Finder iCloud", "A verification code was sent to your trusted devices. Enter it to sign in."));
        m_textButton->setVisible(!texted);
        m_code->clear();
        m_code->setFocus();
        m_codeError->setText(step.message);
        m_codeError->setVisible(!step.message.isEmpty());
        return;
    }
    case FinderCloud::Step::Choice:
        m_pages->setCurrentIndex(2);
        while (QLayoutItem *item = m_phones->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (const auto &[value, label] : step.choices) {
            const bool voice = value.endsWith(QLatin1String("_voice"));
            auto *button = new QPushButton(voice ? i18nc("@action:button Finder iCloud, %1 is a phone number", "Call %1", label)
                                                 : i18nc("@action:button Finder iCloud, %1 is a phone number", "Text %1", label),
                                           m_pages->widget(2));
            connect(button, &QPushButton::clicked, this, [this, value]() {
                sendCode(value);
            });
            m_phones->addWidget(button);
        }
        return;
    case FinderCloud::Step::Error:
        m_signingIn = false;
        showError(step.message);
        return;
    }
}

void FinderICloudSheet::showError(const QString &error)
{
    m_pages->setCurrentIndex(0);
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    m_password->setFocus();
    m_password->selectAll();
}

#include "moc_findercloudsheet.cpp"
